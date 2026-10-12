/* leveler.c - iOS7风格水平仪，160x40：左104px图形，右56px数据。
 * SC7A20仅由load_task读I2C，UI消费经六面校准的镜像。
 * X≈-1g：横地平线；Y≈+1g：左白右黑的竖地平线；
 * Z≈-1g：白色双圆交叠（重叠部分为黑），全程仅使用黑白。
 */
#include "leveler.h"
#include "functions.h"
#include "applications.h"  /* 统一包含算法、驱动的已验证工程路径 */
#include "framework/pm_api.h" /* 水平仪运行期间禁止自动休眠 */
#include <math.h>
#include <string.h>

#define LEVEL_W           104
#define LEVEL_H           40
#define LEVEL_PIXELS      (LEVEL_W * LEVEL_H) /* A8: 3520字节；绘制器已支持A8 */
#define LEVEL_PERIOD_MS   40U            /* UI 25FPS，LOAD 25Hz */
#define LEVEL_R           12             /* 铅锤双圆半径12px */

static lv_obj_t *s_screen, *s_canvas, *s_label[4], *s_degree;
static lv_timer_t *s_timer;
/* 原I2索引画布在当前LVGL软件渲染配置下未启用，故左侧始终全黑。
 * 改用已在小恐龙贴图上验证的A8透明度格式，背景黑、255为白。 */
static uint32_t s_canvas_buf[(LEVEL_PIXELS + 3) / 4];
static lv_draw_buf_t s_canvas_draw_buf;
static char s_text[4][9] = {"X 0.00g", "Y 0.00g", "Z 0.00g", "  0.00"};
static float s_ax, s_ay, s_az;
static uint32_t s_sample_seq;
static uint8_t s_sample_valid, s_mode;
/* 0: X方向地平线；1: Y方向地平线；2: Z轴铅锤双圆
 * X/Y/Z使用实测重力向量，允许在45°切换视图。 */
static const float s_to_deg = 57.2957795f;

/* 直接写A8像素：每帧先清零再重绘，避免透明A8留下上一帧伪影。 */
static void pixel(uint8_t x, uint8_t y)
{
    ((uint8_t *)s_canvas_buf)[(uint16_t)y * LEVEL_W + x] = 255U;
}
static void tick_pixel(uint8_t x, uint8_t y)
{
    /* 刻线与所在区域反色：即使覆盖白色地面也能看清。 */
    ((uint8_t *)s_canvas_buf)[(uint16_t)y * LEVEL_W + x] ^= 255U;
}

/* 横向地平线：重力投影决定旋转方向（不再叠加一遍显示角），
 * Z分量决定线心离屏幕中心的距离：抬头/低头时线心上下移动。
 * X=-1g → 上黑下白；Y=+1g → 左白右黑。 */
/* X=-1g上白下黑，Y=+1g左白右黑；修正旧法向量的X符号。 */
static void render_graphics(float xg, float yg, float zg, float plumb_deg)
{
    int16_t x, y;
    float h = sqrtf(xg * xg + yg * yg);
    /* 每帧全清A8画布，地平线/双圆相交时不会残留旧图像。 */
    memset(s_canvas_buf, 0, sizeof(s_canvas_buf));
    if (s_mode == 2U) {
        /* 双圆的连线相对旧版旋转90°，垂直于XY重力方向。
         * 45°时分离、靠近Z轴时逐渐完全重合；防止上下边界裁切。 */
        float gap = plumb_deg * (2.0f * LEVEL_R) / 45.0f;
        float dx = 0.0f, dy = 0.0f;
        int16_t x1, y1, x2, y2;
        if (gap > 2.0f * LEVEL_R) gap = 2.0f * LEVEL_R;
        if (h > 0.0001f) {
            float ux = -yg / h, uy = xg / h;
            float max_y_gap = LEVEL_H - 1.0f - 2.0f * LEVEL_R;
            if (fabsf(uy) * gap > max_y_gap)
                gap = max_y_gap / fabsf(uy);
            dx = gap * ux;
            dy = gap * uy;
        }
        /* 圆心使用双倍像素坐标，以半像素中心画24px直径的圆。 */
        x1 = (LEVEL_W - 1) - (int16_t)roundf(dx);
        y1 = (LEVEL_H - 1) - (int16_t)roundf(dy);
        x2 = (LEVEL_W - 1) + (int16_t)roundf(dx);
        y2 = (LEVEL_H - 1) + (int16_t)roundf(dy);
        for (y = 0; y < LEVEL_H; ++y) {
            for (x = 0; x < LEVEL_W; ++x) {
                int32_t a = (2*x-x1)*(2*x-x1) + (2*y-y1)*(2*y-y1);
                int32_t b = (2*x-x2)*(2*x-x2) + (2*y-y2)*(2*y-y2);
                if ((a < 4*LEVEL_R*LEVEL_R) !=
                    (b < 4*LEVEL_R*LEVEL_R))
                    pixel((uint8_t)x, (uint8_t)y);
            }
        }
    } else if (h > 0.0001f) {
        /* Z分量改变地平线位置，不强制分界线通过画布中心。 */
        float nx = -yg / h, ny = xg / h;
        float offset = 17.0f * zg;
        for (y = 0; y < LEVEL_H; ++y) {
            for (x = 0; x < LEVEL_W; ++x) {
                float f = nx * (x - (LEVEL_W-1)*0.5f) +
                          ny * (y - (LEVEL_H-1)*0.5f) + offset;
                if (f >= 0.0f) pixel((uint8_t)x, (uint8_t)y);
            }
        }
        /* 两侧短划线固定在中线，随黑白背景反色。 */
        for (x = 4; x <= 12; ++x) tick_pixel((uint8_t)x, 20U);
        for (x = LEVEL_W - 13; x < LEVEL_W - 4; ++x)
            tick_pixel((uint8_t)x, 20U);
    }
    lv_obj_invalidate(s_screen);
}

/* 格式固定：X-1.00g / Y 0.00g / Z 0.00g；统一小数点列。 */
static void print_g(uint8_t n, char axis, float g)
{
    char *t = s_text[n];
    uint16_t num;
    if (g > 9.99f) g = 9.99f;
    if (g < -9.99f) g = -9.99f;
    t[0] = axis;
    num = (uint16_t)(fabsf(g) * 100.0f + 0.5f);
    t[1] = (g < 0.0f && num != 0U) ? '-' : ' ';
    t[2] = (char)('0' + num / 100U);
    t[3] = '.';
    t[4] = (char)('0' + (num / 10U) % 10U);
    t[5] = (char)('0' + num % 10U);
    t[6] = 'g'; t[7] = '\0';
    lv_obj_invalidate(s_label[n]);
}
static void print_angle(float angle)
{
    uint16_t n;
    char *t = s_text[3];
    if (angle < 0.0f) angle = -angle;
    if (angle > 99.99f) angle = 99.99f;
    n = (uint16_t)(angle * 100.0f + 0.5f);
    t[0] = ' '; t[1] = n >= 1000U ? (char)('0' + n / 1000U) : ' ';
    t[2] = (char)('0' + n / 100U % 10U);
    t[3] = '.';
    t[4] = (char)('0' + (n / 10U) % 10U);
    t[5] = (char)('0' + n % 10U);
    t[6] = '\0';
    lv_obj_invalidate(s_label[3]);
}

static void level_tick(lv_timer_t *timer)
{
    float x, y, z, angle, h;
    uint32_t seq;
    (void)timer;
    if (!SC7A20_AlgoLevelerGet(&x, &y, &z, &seq)) return;
    if (seq == s_sample_seq) return;
    s_sample_seq = seq;
    if (!s_sample_valid) {
        s_ax = x; s_ay = y; s_az = z; s_sample_valid = 1U;
    } else {
        s_ax += 0.2f * (x - s_ax); /* EMA，平滑气泡且不积累持久误差 */
        s_ay += 0.2f * (y - s_ay);
        s_az += 0.2f * (z - s_az);
    }
    x = s_ax * 0.001f; y = s_ay * 0.001f; z = s_az * 0.001f;

    /* Z与屏幕平面重力分量等大即45°：水平地平线↔铅锤双圆。
     * 只用同一组重力分量求方向与角度，不随X/Y切换叠加角度。 */
    h = sqrtf(x * x + y * y);
    if (fabsf(z) >= h) {
        s_mode = 2U;
        angle = atan2f(h, fabsf(z)) * s_to_deg; /* Z轴铅锤偏差 */
    } else {
        s_mode = fabsf(y) > fabsf(x) ? 1U : 0U;
        /* 水平偏差在X/Y两个正交基准下取最近方向，避免累计成双倍角。 */
        angle = (s_mode == 0U ? atan2f(fabsf(y), fabsf(x)) :
                 atan2f(fabsf(x), fabsf(y))) * s_to_deg;
    }
    render_graphics(x, y, z, angle);
    print_g(0U, 'X', x); print_g(1U, 'Y', y); print_g(2U, 'Z', z);
    print_angle(angle);
    /* 角度符号始终白色，不随平衡状态变色。 */
}

/* 水平仪在运行期间持续采样、禁止进入DeepSleep；
 * 进入/退出仍通过load_task切换50Hz/25Hz ODR，UI不直接执行I2C。 */
static void level_create(void)
{
    uint8_t i;
    if (s_screen) return;
    s_sample_valid = 0U; s_sample_seq = 0U;
    s_mode = 0U;
    /* 重入不沿用上一次的临时数值，等待load_task刷新三轴镜像。 */
    memcpy(s_text[0], "X 0.00g", 8);
    memcpy(s_text[1], "Y 0.00g", 8);
    memcpy(s_text[2], "Z 0.00g", 8);
    memcpy(s_text[3], "  0.00", 7);
    pm_api_set_sleep_block(PM_BLOCK_LEVELER, 1U);
    pm_api_refresh_idle();
    SC7A20_AlgoLevelerSet(1U);
    ui_orientation_lock_set(1U);
    s_screen = lv_obj_create(NULL);
    lv_obj_set_size(s_screen, 160, 40);
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_screen, LV_SCROLLBAR_MODE_OFF);

    s_canvas = lv_canvas_create(s_screen);
    /* 直接用A8源像素（尺寸104x40），配白色image_recolor。
     * A8=0透出黑色底板，A8=255显示纯白，不需要调色板。 */
    lv_draw_buf_init(&s_canvas_draw_buf, LEVEL_W, LEVEL_H, LV_COLOR_FORMAT_A8,
                     LEVEL_W, s_canvas_buf, sizeof(s_canvas_buf));
    lv_canvas_set_draw_buf(s_canvas, &s_canvas_draw_buf);
    lv_obj_set_style_image_recolor(s_canvas, lv_color_white(), 0);
    lv_obj_set_style_image_recolor_opa(s_canvas, LV_OPA_COVER, 0);
    lv_obj_set_pos(s_canvas, 0, 0);

    /* 数据区7字符×8px恰好56px，固定黑底与图形区拼接。 */
    for (i = 0U; i < 4U; ++i) {
        s_label[i] = lv_label_create(s_screen);
        lv_obj_set_pos(s_label[i], LEVEL_W, 1 + 10 * i);
        lv_obj_set_size(s_label[i], 160 - LEVEL_W, 9);
        lv_obj_set_style_pad_all(s_label[i], 0, 0);
        lv_obj_set_style_bg_opa(s_label[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_label[i], lv_color_black(), 0);
        lv_obj_set_style_text_color(s_label[i], lv_color_white(), 0);
        lv_obj_set_style_text_font(s_label[i], &lv_font_unscii_8, 0);
        lv_label_set_long_mode(s_label[i], LV_LABEL_LONG_MODE_CLIP);
        lv_label_set_text_static(s_label[i], s_text[i]);
    }
    /* UNSCII-8仅覆盖ASCII，角度符号单独使用4px白色细圆环。 */
    s_degree = lv_obj_create(s_screen);
    lv_obj_set_size(s_degree, 4, 4);
    lv_obj_set_pos(s_degree, LEVEL_W + 49, 31);
    lv_obj_set_style_radius(s_degree, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_degree, 1, 0);
    lv_obj_set_style_border_color(s_degree, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_degree, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_degree, 0, 0);

    s_timer = lv_timer_create(level_tick, LEVEL_PERIOD_MS, NULL);
    lv_timer_pause(s_timer);
    render_graphics(-1.0f, 0.0f, 0.0f, 0.0f);
}
static void level_activate(void)
{
    if (!s_screen) return;
    lv_screen_load(s_screen);
    lv_obj_invalidate(s_screen);
    lv_timer_resume(s_timer);
}
static void level_run(app_action_t action)
{
    /* MENU、NEXT、CONF以及三键双击/长按均只执行退出，不再归零。 */
    if (action != APP_ACTION_NONE) menu_app_exit();
}
static void level_destroy(void)
{
    SC7A20_AlgoLevelerSet(0U);
    pm_api_set_sleep_block(PM_BLOCK_LEVELER, 0U);
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    if (s_screen) { lv_obj_delete(s_screen); s_screen = NULL; }
    s_canvas = s_degree = NULL;
    for (uint8_t i = 0U; i < 4U; ++i) s_label[i] = NULL;
    ui_orientation_lock_set(0U);
}
const menu_app_t menu_app_leveler = {
    .label = "Leveler",
    .create = level_create, .activate = level_activate,
    .run = level_run, .destroy = level_destroy
};
