/* dinosaur_game.c - 160x40 LVGL 小恐龙
 * 参考：https://github.com/hjl240/STM32DinoGame
 * MENU 退出不清进度；NEXT/CONF 跳跃，生命归零后按键重新开局。
 * 图像资源为 dinosaur_icon.c 中的 A8，固定对象池，最高分仅 RAM。
 */
#include "dinosaur_game.h"
#include "dinosaur_icon.h"
#include "functions.h"
#include "framework/pm_api.h"
#include "at32f423.h"

#define DINO_W             160
#define DINO_GROUND_Y       32
#define DINO_X               6
#define DINO_Y              16
#define DINO_GROUND_W      597
#define DINO_TICK_MS        33
#define DINO_CACTUS_N        3
#define DINO_CLOUD_N         4

typedef struct { int16_t x; uint8_t type, active, hit; } cactus_t;
typedef struct { int16_t x; uint8_t y, div, phase, active; } cloud_t;
static struct {
    uint32_t distance, best, rng;
    int16_t y2, cactus_gap, cloud_gap; /* y2 为半像素坐标 */
    int8_t vy2;                       /* 半像素/帧；重力为 +1 */
    uint8_t life, dead, ready, frame;
    cactus_t cactus[DINO_CACTUS_N];
    cloud_t cloud[DINO_CLOUD_N];
} g;

/* 页面关闭时对象释放，但 g 不清零，重新进入可接着玩。 */
static lv_obj_t *screen, *hud, *player, *ground[2], *over[2];
static lv_obj_t *cactus_img[DINO_CACTUS_N], *cloud_img[DINO_CLOUD_N];
static lv_timer_t *timer;

static void seed_rng(void)
{
    /* 系统毫秒值 + SysTick 子毫秒计数，避免原版 RTC 日期之和导致固定关卡。
     * 独立 xorshift，不影响其它模块的 srand/rand 全局状态，不产生 I2C 操作。 */
    g.rng ^= lv_tick_get() ^ SysTick->VAL ^ 0x9E3779B9UL;
    if (!g.rng) g.rng = 0x6D2B79F5UL;
}
static uint32_t next_rand(void)
{
    uint32_t v = g.rng;
    v ^= v << 13; v ^= v >> 17; v ^= v << 5;
    return g.rng = v;
}
static uint8_t world_speed(void)
{
    uint32_t s = g.distance / 2U;
    return s >= 600U ? 5U : s >= 300U ? 4U : s >= 100U ? 3U : 2U;
}
static void reset_game(void)
{
    uint8_t i;
    g.distance = 0;
    g.y2 = 2 * DINO_Y;
    g.vy2 = 0;
    g.cactus_gap = 80;
    g.cloud_gap = 55;
    g.life = 3;
    g.dead = 0;
    g.frame = 0;
    for (i = 0; i < DINO_CACTUS_N; ++i) g.cactus[i].active = 0;
    for (i = 0; i < DINO_CLOUD_N; ++i) g.cloud[i].active = 0;
    seed_rng();
    g.ready = 1;
}
static lv_obj_t *sprite(const lv_image_dsc_t *src)
{
    lv_obj_t *o = lv_image_create(screen);
    lv_image_set_src(o, src);
    lv_obj_set_style_image_recolor(o, lv_color_white(), 0);
    lv_obj_set_style_image_recolor_opa(o, LV_OPA_COVER, 0);
    return o;
}
static lv_obj_t *label(const char *str, int16_t x, int16_t y, int16_t width)
{
    lv_obj_t *o = lv_label_create(screen);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_text_color(o, lv_color_white(), 0);
    lv_obj_set_style_text_font(o, menu_font_main(), 0); /* u14b，8px/字符 */
    lv_label_set_long_mode(o, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_size(o, width, 14);
    lv_obj_set_pos(o, x, y);
    lv_label_set_text(o, str);
    return o;
}
static void five_digits(char *p, uint32_t n)
{
    uint8_t k;
    if (n > 99999U) n = 99999U;
    for (k = 5; k > 0; --k) {
        p[k-1] = (char)('0' + n % 10U);
        n /= 10U;
    }
}
static void draw_game(void)
{
    static const lv_image_dsc_t *const shapes[4] = {
        &dino_icon_cactus0, &dino_icon_cactus1,
        &dino_icon_cactus2, &dino_icon_cactus3
    };
    uint8_t i;
    int16_t shift;
    char title[] = "*** HI00000 00000"; /* 17字 × 8px = 136px，右对齐 */
    if (!screen) return;
    for (i = g.life; i < 3U; ++i) title[i] = ' ';
    five_digits(title + 6, g.best);
    five_digits(title + 12, g.distance / 2U);
    lv_label_set_text(hud, title);

    shift = (int16_t)(g.distance % DINO_GROUND_W);
    lv_obj_set_pos(ground[0], -shift, DINO_GROUND_Y);
    if (shift > DINO_GROUND_W - DINO_W) {
        lv_obj_remove_flag(ground[1], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(ground[1], DINO_GROUND_W - shift, DINO_GROUND_Y);
    } else lv_obj_add_flag(ground[1], LV_OBJ_FLAG_HIDDEN);

    for (i = 0; i < DINO_CACTUS_N; ++i) {
        cactus_t *c = &g.cactus[i];
        if (c->active) {
            lv_obj_remove_flag(cactus_img[i], LV_OBJ_FLAG_HIDDEN);
            lv_image_set_src(cactus_img[i], shapes[c->type]);
            lv_obj_set_pos(cactus_img[i], c->x, DINO_Y);
        } else lv_obj_add_flag(cactus_img[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (i = 0; i < DINO_CLOUD_N; ++i) {
        cloud_t *c = &g.cloud[i];
        if (c->active) {
            lv_obj_remove_flag(cloud_img[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(cloud_img[i], c->x, c->y);
        } else lv_obj_add_flag(cloud_img[i], LV_OBJ_FLAG_HIDDEN);
    }

    lv_image_set_src(player, g.dead ? &dino_icon_end :
                     g.y2 < 2 * DINO_Y ? &dino_icon_jump :
                     (g.frame & 4U) ? &dino_icon_run1 : &dino_icon_run0);
    lv_obj_set_pos(player, DINO_X, g.y2 / 2);
    for (i = 0; i < 2U; ++i) {
        if (g.dead) lv_obj_remove_flag(over[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(over[i], LV_OBJ_FLAG_HIDDEN);
    }
}
static void frame_tick(lv_timer_t *t)
{
    static const uint8_t cactus_w[4] = {8U, 16U, 24U, 24U};
    uint8_t i, sp;
    (void)t;
    if (g.dead) return;
    sp = world_speed();
    g.distance += sp;
    ++g.frame;
    if (g.distance / 2U > g.best) g.best = g.distance / 2U;

    /* 最高点 vy2=0 时也必须继续积分，否则会停在空中。 */
    if (g.vy2 != 0 || g.y2 < 2 * DINO_Y) {
        g.y2 += g.vy2++;
        if (g.y2 >= 2 * DINO_Y) { g.y2 = 2 * DINO_Y; g.vy2 = 0; }
    }

    for (i = 0; i < DINO_CACTUS_N; ++i) {
        cactus_t *c = &g.cactus[i];
        if (!c->active) continue;
        c->x -= sp;
        if (c->x < -(int16_t)cactus_w[c->type]) { c->active = 0; continue; }
        /* 一个障碍最多扣一命；恐龙离地 8px 后可越过。 */
        if (!c->hit && c->x < DINO_X + 15 &&
            c->x + cactus_w[c->type] > DINO_X + 1 && g.y2 > 16) {
            c->hit = 1;
            if (--g.life == 0) {
                g.dead = 1;
                g.y2 = 2 * DINO_Y;
                g.vy2 = 0;
                lv_timer_pause(timer);
                pm_api_set_sleep_block(PM_BLOCK_DINO, 0);
                break;
            }
        }
    }
    if (!g.dead) {
        g.cactus_gap -= sp;
        if (g.cactus_gap <= 0) {
            for (i = 0; i < DINO_CACTUS_N; ++i) {
                cactus_t *c = &g.cactus[i];
                if (!c->active) {
                    c->active = 1; c->hit = 0; c->x = DINO_W;
                    c->type = g.distance < 200U ? 0U :
                              g.distance < 600U ? next_rand() % 2U :
                              next_rand() % 4U;
                    break;
                }
            }
            g.cactus_gap = 96 + (int16_t)(next_rand() % 112U);
        }
        for (i = 0; i < DINO_CLOUD_N; ++i) {
            cloud_t *c = &g.cloud[i];
            if (!c->active) continue;
            if (++c->phase >= c->div) { c->phase = 0; c->x -= sp; }
            if (c->x < -24) c->active = 0;
        }
        g.cloud_gap -= sp;
        if (g.cloud_gap <= 0) {
            for (i = 0; i < DINO_CLOUD_N; ++i) {
                cloud_t *c = &g.cloud[i];
                if (!c->active) {
                    c->active = 1; c->x = DINO_W; c->phase = 0;
                    c->y = 14U + next_rand() % 7U;
                    c->div = 2U + next_rand() % 3U;
                    break;
                }
            }
            g.cloud_gap = 50 + (int16_t)(next_rand() % 180U);
        }
    }
    draw_game();
}
static void game_create(void)
{
    uint8_t i;
    if (screen) return;
    if (!g.ready) reset_game();
    screen = lv_obj_create(NULL);
    lv_obj_set_size(screen, DINO_W, 40);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101010), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    ui_orientation_lock_set(1);
    ground[0] = sprite(&dino_icon_ground);
    ground[1] = sprite(&dino_icon_ground);
    for (i = 0; i < DINO_CLOUD_N; ++i) cloud_img[i] = sprite(&dino_icon_cloud);
    for (i = 0; i < DINO_CACTUS_N; ++i) cactus_img[i] = sprite(&dino_icon_cactus0);
    player = sprite(&dino_icon_run0);
    hud = label("*** HI00000 00000", 24, -1, 136);
    over[0] = label("GAME OVER", 80, 13, 80);
    over[1] = label("NEXT/CONF", 80, 26, 80);
    for (i = 0; i < 2; ++i) {
        lv_obj_set_style_bg_color(over[i], lv_color_hex(0x101010), 0);
        lv_obj_set_style_bg_opa(over[i], LV_OPA_COVER, 0);
    }
    timer = lv_timer_create(frame_tick, DINO_TICK_MS, NULL);
    lv_timer_pause(timer); /* 过渡动画期间不推进游戏 */
    pm_api_set_sleep_block(PM_BLOCK_DINO, g.dead ? 0 : 1);
    draw_game();
}
static void game_activate(void)
{
    if (!screen) return;
    lv_screen_load(screen);
    lv_obj_invalidate(screen);
    if (timer && !g.dead) lv_timer_resume(timer);
}
static void game_run(app_action_t action)
{
    if (action == APP_ACTION_UP) {
        if (timer) lv_timer_pause(timer);
        menu_app_exit();
        return;
    }
    if (action != APP_ACTION_DOWN && action != APP_ACTION_ENTER &&
        action != APP_ACTION_DOWN_DBL && action != APP_ACTION_ENTER_DBL) return;
    if (g.dead) {
        reset_game(); /* 只有死亡后主动按键才清进度；best 不清 */
        pm_api_set_sleep_block(PM_BLOCK_DINO, 1);
        if (timer) lv_timer_resume(timer);
        draw_game();
    } else if (g.y2 >= 2 * DINO_Y) {
        g.vy2 = -7; /* 固定跳高，与按键时长无关 */
    }
}
static void game_destroy(void)
{
    uint8_t i;
    pm_api_set_sleep_block(PM_BLOCK_DINO, 0);
    if (timer) { lv_timer_delete(timer); timer = NULL; }
    if (screen) { lv_obj_delete(screen); screen = NULL; }
    hud = player = NULL;
    ground[0] = ground[1] = NULL;
    over[0] = over[1] = NULL;
    for (i = 0; i < DINO_CACTUS_N; ++i) cactus_img[i] = NULL;
    for (i = 0; i < DINO_CLOUD_N; ++i) cloud_img[i] = NULL;
    ui_orientation_lock_set(0);
}
const menu_app_t menu_app_dino = {
    .label = "Dino Game",
    .create = game_create, .activate = game_activate,
    .run = game_run, .destroy = game_destroy,
};
