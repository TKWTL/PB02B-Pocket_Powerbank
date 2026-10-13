/* dinosaur_game.c - 160x40 LVGL 小恐龙
 * 参考：https://github.com/hjl240/STM32DinoGame
 * MENU 退出不清进度；NEXT/CONF 跳跃，生命归零后按键重新开局。
 * 图像资源为 dinosaur_icon.c 中的 A8，固定对象池，最高分仅 RAM。
 */
#include "dinosaur_game.h"
#include "dinosaur_icon.h"
#include "functions.h"
#include "menu.h"              /* menu_tr：结束界面文案的 i18n */
#include "framework/pm_api.h"
#include "at32f423.h"
#include <math.h>

#define DINO_W             160
#define DINO_GROUND_Y       33
#define DINO_X               6
#define DINO_Y              22
#define DINO_CACTUS_Y       23
#define DINO_JUMP_VY2       -9
/* 原 XBM 碰撞判据留 4px 容差；恐龙与仙人掌各高 16px。 */
#define DINO_SAFE_Y2 (2 * (DINO_CACTUS_Y - 16 + 4))
#define DINO_GROUND_W      597
#define DINO_TICK_MS        33
#define DINO_CACTUS_N        3
#define DINO_CLOUD_N         3
#define DINO_SPEED_Q          12U   /* 1/4096 px/frame；平滑分摊每帧的整数像素 */
#define DINO_SKY_FADE_FRAMES 42U    /* 42×33ms≈1.39s渐变 */
#define DINO_SKY_WAIT_MIN    1200U  /* 最短：原120帧×10 = 39.6s */
#define DINO_SKY_WAIT_RAND   7181U  /* 最长：1200+7180=8380帧，原419帧×20 */

typedef struct { int16_t x; uint8_t type, active, hit; } cactus_t;
typedef struct { int16_t x; int8_t y; uint8_t div, phase, active; } cloud_t;
static struct {
    uint32_t distance, best, rng;
    uint16_t distance_frac;         /* 速度的 Q12 小数累积，MENU 退出保留 */
    int16_t y2, cactus_gap, cloud_gap; /* y2 为半像素坐标 */
    int8_t vy2;                       /* 半像素/帧；重力为 +1 */
    uint16_t sky_wait;
    int8_t sky_step;             /* -1 向夜，+1 向昼，0 保持 */
    uint8_t sky_level;           /* 0=夜，DINO_SKY_FADE_FRAMES=昼；不参与游戏物理 */
    uint8_t life, dead, ready, frame, hi_crossed, hi_blink;
    cactus_t cactus[DINO_CACTUS_N];
    cloud_t cloud[DINO_CLOUD_N];
} g;

/* 页面关闭时对象释放，但 g 不清零，重新进入可接着玩。 */
static lv_obj_t *screen, *hud, *player, *ground[2], *over[2];
static lv_obj_t *cactus_img[DINO_CACTUS_N], *cloud_img[DINO_CLOUD_N];
static lv_timer_t *timer;
static uint8_t palette_level = 0xFFU, palette_mode = 0xFFU;

static void seed_rng(void)
{
    /* AT32F423参考手册§1.3：96-bit UID从0x1FFFF7E8起。
     * UID本身固定，并非真随机数；混入系统毫秒和SysTick计数增加变化。
     * 独立xorshift，不影响其它模块的rand，也不访问I2C/RTC。 */
    const volatile uint32_t *uid = (const volatile uint32_t *)0x1FFFF7E8UL;
    g.rng ^= uid[0] ^ (uid[1] << 11 | uid[1] >> 21) ^
             (uid[2] << 23 | uid[2] >> 9) ^
             lv_tick_get() ^ SysTick->VAL ^ 0x9E3779B9UL;
    if (!g.rng) g.rng = 0x6D2B79F5UL;
}
static uint32_t next_rand(void)
{
    uint32_t v = g.rng;
    v ^= v << 13; v ^= v >> 17; v ^= v << 5;
    return g.rng = v;
}
/* 32-bit 整数平方根：仅在30FPS速度计算中调用，无float/libm。 */
static uint16_t speed_isqrt(uint32_t n)
{
    uint32_t root = 0U, bit = 1UL << 30;
    while (bit > n) bit >>= 2;
    while (bit) {
        if (n >= root + bit) {
            n -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return (uint16_t)root;
}

/* 横抛物线开方 → 谷底后加速 → 切线连续的开方曲线。
 * s=游戏分数，速度Q12单位为px/帧：
 *   0..100:  2 + 0.045*(sqrt(s+100)-10)
 * 100..400:  2.1863 + 0.00159*d + 1.514e-6*d² (d=s-100)
 * 400..5000:2.8000 + 0.03606*(sqrt(s-400+52)-sqrt(52))
 * 最大斜率约0.0025px/帧/分；5000分后接Hermite曲线，
 * 12000/30000分分别6/7px每帧，之后无封顶开方增长。
 * 前5000分保留定点整数平方根，后段使用Cortex-M4F单精度FPU。 */
static uint32_t world_speed_q12(void)
{
    uint32_t s = g.distance / 2U, d, root;
    if (s <= 100U) {
        root = speed_isqrt((s + 100U) << 16);
        return (uint16_t)(8192U + (720U * (root - 2560U) + 500U) / 1000U);
    }
    if (s <= 400U) {
        d = s - 100U;
        return (uint16_t)(8955U + (d * 652U + 50U) / 100U +
                          (d * d * 62U + 5000U) / 10000U);
    }
    if (s <= 5000U) {
        root = speed_isqrt((s - 400U + 52U) << 16);
        return (uint32_t)(11469U + (577U * (root - 1846U) + 500U) / 1000U);
    }
    /* Cortex-M4F：用单精度硬件浮点计算后段平滑Hermite/SQRT。
     * 5000/12000/30000分 = 5/6/7 px/帧；斜率在连接处连续。
     * 30000以后仍按sqrt增长，不再设速度封顶。 */
    {
        float x, h, t, t2, t3, v, m0, m1;
        if (s <= 30000U) {
            if (s < 12000U) {
                x = (float)(s - 5000U); h = 7000.0f;
                m0 = 0.000264f; m1 = 0.000080f;
                v = 5.0f;  /* 左端点 */
            } else {
                x = (float)(s - 12000U); h = 18000.0f;
                m0 = 0.000080f; m1 = 0.000040f;
                v = 6.0f;
            }
            t = x / h; t2 = t * t; t3 = t2 * t;
            v += (-2.0f*t3 + 3.0f*t2)
                 + (t3 - 2.0f*t2 + t)*h*m0 + (t3-t2)*h*m1;
        } else {
            v = 7.0f + 1.44f * (sqrtf(1.0f +
                    (float)(s - 30000U)/18000.0f) - 1.0f);
        }
        return (uint32_t)(v * (float)(1U << DINO_SPEED_Q) + 0.5f);
    }
}
static void reset_game(void)
{
    uint8_t i;
    g.distance = 0;
    g.distance_frac = 0;
    g.y2 = 2 * DINO_Y;
    g.vy2 = 0;
    g.cactus_gap = 80;
    g.cloud_gap = 55;
    g.life = 3;
    g.dead = 0;
    g.frame = 0;
    g.hi_crossed = 0;
    g.hi_blink = 0;
    for (i = 0; i < DINO_CACTUS_N; ++i) g.cactus[i].active = 0;
    for (i = 0; i < DINO_CLOUD_N; ++i) g.cloud[i].active = 0;
    seed_rng();
    /* 新一局随机初始昼夜与切换时间；MENU退出/返回不调用reset_game。 */
    g.sky_level = (next_rand() & 1U) ? DINO_SKY_FADE_FRAMES : 0U;
    g.sky_step = 0;
    g.sky_wait = DINO_SKY_WAIT_MIN + next_rand() % DINO_SKY_WAIT_RAND;
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
/* 原来的 -9/+1 半像素运动保持19帧完整周期，显示振幅扩大4/3。
 * 最高点sprite y=-8，屏幕内仅剩恐龙底部8px。 */
static int16_t player_screen_y(void)
{
    return DINO_Y - (int16_t)((2 * DINO_Y - g.y2) * 2 / 3);
}

/* 只在亮度变化时修改样式。背景灰度渐变；前景中点切换确保始终
 * 有足够对比度，不会在灰度交叉时让文字/仙人掌变成不可见。
 * 云始终使用灰阶色；死亡提示保留字体但背景随天空变化。
 * 图像先于HUD创建，HUD始终叠在云朵上方。 */
static void update_palette(void)
{
    uint8_t i, is_day, gray;
    lv_color_t bg, ink, cloud;
    if (palette_level == g.sky_level) return;
    palette_level = g.sky_level;
    gray = (uint8_t)(18U + (uint32_t)g.sky_level * 218U / DINO_SKY_FADE_FRAMES);
    bg = lv_color_make(gray, gray, gray);
    lv_obj_set_style_bg_color(screen, bg, 0);
    for (i = 0; i < 2U; ++i) lv_obj_set_style_bg_color(over[i], bg, 0);

    is_day = g.sky_level > DINO_SKY_FADE_FRAMES / 2U;
    if (palette_mode == is_day) return;
    palette_mode = is_day;
    ink = lv_color_hex(is_day ? 0x181818 : 0xF0F0F0);
    cloud = lv_color_hex(is_day ? 0x555555 : 0xBFBFBF);
    lv_obj_set_style_text_color(hud, ink, 0);
    for (i = 0; i < 2U; ++i) {
        lv_obj_set_style_image_recolor(ground[i], ink, 0);
        lv_obj_set_style_text_color(over[i], ink, 0);
    }
    lv_obj_set_style_image_recolor(player, ink, 0);
    for (i = 0; i < DINO_CACTUS_N; ++i)
        lv_obj_set_style_image_recolor(cactus_img[i], ink, 0);
    for (i = 0; i < DINO_CLOUD_N; ++i)
        lv_obj_set_style_image_recolor(cloud_img[i], cloud, 0);
}

/* 仅装饰性昼夜交替，不变更障碍、碰撞、分数或速度。
 * 用帧数计时：菜单退出/DeepSleep后自然冻结，回到游戏继续。 */
static void update_sky(void)
{
    if (g.sky_step) {
        g.sky_level = (uint8_t)((int16_t)g.sky_level + g.sky_step);
        if (g.sky_level == 0U || g.sky_level == DINO_SKY_FADE_FRAMES) {
            g.sky_step = 0;
            g.sky_wait = DINO_SKY_WAIT_MIN + next_rand() % DINO_SKY_WAIT_RAND;
        }
    } else if (g.sky_wait) {
        --g.sky_wait;
    } else {
        g.sky_step = g.sky_level ? -1 : 1;
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
    char title[] = "*** HI 00000  00000"; /* 19字 × 8px = 152px，右对齐 */
    if (!screen) return;
    update_palette();
    for (i = g.life; i < 3U; ++i) title[i] = ' ';
    five_digits(title + 7, g.best);
    five_digits(title + 14, g.distance / 2U);
    /* 创纪录时HI与旧纪录闪烁三次（每半周期6帧），其它HUD保持正常。 */
    if (g.hi_blink && (((g.hi_blink - 1U) / 6U) & 1U))
        for (i = 4; i < 12; ++i) title[i] = ' ';
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
            lv_obj_set_pos(cactus_img[i], c->x, DINO_CACTUS_Y);
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
    lv_obj_set_pos(player, DINO_X, player_screen_y());
    for (i = 0; i < 2U; ++i) {
        if (g.dead) lv_obj_remove_flag(over[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(over[i], LV_OBJ_FLAG_HIDDEN);
    }
    /* A8 精灵的透明区必须显示本帧背景，避免移动精灵、地面留下残影。
     * 根屏背景已设置 LV_OPA_COVER，整屏 invalidate 可重绘全部 160x40。
     * 只在 draw_game 被调用时执行，不创建额外全屏缓存或对象。 */
    lv_obj_invalidate(screen);
}
static void frame_tick(lv_timer_t *t)
{
    static const uint8_t cactus_w[4] = {8U, 16U, 24U, 24U};
    uint8_t i;
    uint16_t sp;
    uint32_t step;
    (void)t;
    if (g.dead) return;
    update_sky();
    /* 累积亚像素位移，避免 Q12 速度直接取整又形成新档位。
     * 障碍、地面及云朵共用这一帧位移，碰撞与画面保持同步。 */
    step = g.distance_frac + world_speed_q12();
    sp = (uint16_t)(step >> DINO_SPEED_Q);
    g.distance_frac = step & ((1U << DINO_SPEED_Q) - 1U);
    g.distance += sp;
    ++g.frame;
    /* 历史最高分直到生命耗尽才更新；首次超越历史纪录闪烁三次。 */
    if (!g.hi_crossed && g.distance / 2U > g.best) {
        g.hi_crossed = 1U;
        g.hi_blink = 36U;
    }

    /* 最高点vy2=0时继续积分；维持既有19帧跳跃周期。 */
    if (g.vy2 != 0 || g.y2 < 2 * DINO_Y) {
        g.y2 += g.vy2++;
        if (g.y2 >= 2 * DINO_Y) { g.y2 = 2 * DINO_Y; g.vy2 = 0; }
    }

    for (i = 0; i < DINO_CACTUS_N; ++i) {
        cactus_t *c = &g.cactus[i];
        if (!c->active) continue;
        c->x -= sp;
        if (c->x < -(int16_t)cactus_w[c->type]) { c->active = 0; continue; }
        /* 碰撞高度与实际显示位置一致，避免已跳过障碍仍被扣命。 */
        if (!c->hit && c->x < DINO_X + 15 &&
            c->x + cactus_w[c->type] > DINO_X + 1 &&
            player_screen_y() > DINO_SAFE_Y2 / 2) {
            c->hit = 1;
            if (--g.life == 0) {
                g.dead = 1;
                g.y2 = 2 * DINO_Y;
                g.vy2 = 0;
                if (g.distance / 2U > g.best) g.best = g.distance / 2U;
                g.hi_blink = 0;
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
            /* 最小间距随速度增长，确保下一棵仙人掌不会挤进19帧跳跃窗口。 */
            {
                uint16_t min_gap = (uint16_t)(19U *
                        ((world_speed_q12() + 4095U) >> DINO_SPEED_Q) + 16U);
                if (min_gap < 96U) min_gap = 96U;
                g.cactus_gap = (int16_t)(min_gap + next_rand() % 112U);
            }
        }
        for (i = 0; i < DINO_CLOUD_N; ++i) {
            cloud_t *c = &g.cloud[i];
            if (!c->active) continue;
            /* 速度为原来一半；保留子像素余数，已有云朵也立即生效。 */
            uint8_t div = (uint8_t)(c->div << 1);
            c->phase += sp;
            c->x -= c->phase / div;
            c->phase %= div;
            if (c->x < -24) c->active = 0;
        }
        g.cloud_gap -= sp;
        if (g.cloud_gap <= 0) {
            for (i = 0; i < DINO_CLOUD_N; ++i) {
                cloud_t *c = &g.cloud[i];
                if (!c->active) {
                    c->active = 1; c->x = DINO_W; c->phase = 0;
                    c->y = (int8_t)((int32_t)(next_rand() % 29U) - 4); /* -4..24：顶端只露4px */
                    c->div = 2U + next_rand() % 3U;
                    break;
                }
            }
            g.cloud_gap = 50 + (int16_t)(next_rand() % 180U);
        }
    }
    draw_game();
    if (g.hi_blink) --g.hi_blink;
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
    hud = label("*** HI 00000  00000", 8, 0, 152);
    lv_obj_set_style_text_font(hud, &lv_font_unscii_8, 0);
    lv_obj_set_height(hud, 9); /* UNSCII-8 行高9，字宽8；17字符仍为136px */
    /* 结束文字两行整体上移3px；重开提示中文再右移4px、英文左移12px。 */
    over[0] = label(menu_tr("GAME OVER"), 88, 10, 72);
    over[1] = (menu_lang_get() == MENU_LANG_ZH) ?
              label(menu_tr("JUMP TO RESTART"), 70, 23, 90) :
              label(menu_tr("JUMP TO RESTART"), 40, 23, 120);
    for (i = 0; i < 2; ++i) {
        lv_obj_set_style_bg_color(over[i], lv_color_hex(0x101010), 0);
        lv_obj_set_style_bg_opa(over[i], LV_OPA_COVER, 0);
    }
    palette_level = palette_mode = 0xFFU; /* 每次重建对象强制同步当前日夜配色 */
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
        g.vy2 = DINO_JUMP_VY2; /* 半像素物理+显示放大，最大上升30px */
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
