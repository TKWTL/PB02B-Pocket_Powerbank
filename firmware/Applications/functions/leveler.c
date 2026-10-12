/* leveler.c - iOS7风格水平仪，160x40：左88px图形，右64px数据。
 * SC7A20仅由load_task读I2C，UI消费经六面校准的镜像。
 * X≈-1g：横地平线；Y≈+1g：左白右黑的竖地平线；
 * Z≈-1g：双圆交叠（重叠区域黑色），水平时绿色高亮。
 */
#include "leveler.h"
#include "functions.h"
#include "sc7a20_algo.h"
#include <math.h>
#include <string.h>

#define LEVEL_W           88
#define LEVEL_H           40
#define LEVEL_STRIDE      (LEVEL_W / 4)  /* 2bpp，一行22字节 */
#define LEVEL_PIXELS      (LEVEL_STRIDE * LEVEL_H)
#define LEVEL_BUF_BYTES   (16 + LEVEL_PIXELS)
#define LEVEL_PERIOD_MS   40U            /* UI 25FPS，LOAD 25Hz */

static lv_obj_t *s_screen, *s_canvas, *s_label[4], *s_degree;
static lv_timer_t *s_timer;
/* I2调色板16字节 + 880字节像素；4字节自然对齐。 */
static uint32_t s_canvas_buf[(LEVEL_BUF_BYTES + 3) / 4];
static char s_text[4][9] = {"X 0.00g", "Y 0.00g", "Z 0.00g", "  0.00"};
static float s_ax, s_ay, s_az, s_zero_deg;
static uint32_t s_sample_seq;
static uint8_t s_sample_valid, s_mode;
/* 0:沿X的横线；1:沿Y的竖线；2:Z轴平放双圆 */
static const float s_to_deg = 57.2957795f;

static void palette_set(uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
    lv_color32_t c;
    c.red = r; c.green = g; c.blue = b; c.alpha = 255;
    lv_canvas_set_palette(s_canvas, index, c);
}

/* 直接写入 I2 索引缓冲，不调用会逐点触发 invalidate 的canvas_set_px。 */
static void pixel(uint8_t x, uint8_t y, uint8_t col)
{
    uint8_t *p = (uint8_t *)s_canvas_buf + 16 + (uint16_t)y * LEVEL_STRIDE + (x >> 2);
    uint8_t shift = (uint8_t)(6U - 2U * (x & 3U));
    *p = (uint8_t)((*p & ~(3U << shift)) | (col << shift));
}

static void render_graphics(float xg, float yg, float zg, float angle)
{
    int16_t x, y, circle_x, circle_y;
    uint8_t balanced = (angle < 2.0f);
    uint8_t color;
    (void)zg;
    memset((uint8_t *)s_canvas_buf + 16, 0, LEVEL_PIXELS);
    if (s_mode == 2U) {
        /* 底部固定圆与重力偏移圆；像素异或形成黑色相交透镜。 */
        circle_x = 50 + (int16_t)(xg * 15.0f);
        circle_y = 25 - (int16_t)(yg * 10.0f);
        for (y = 0; y < LEVEL_H; ++y) {
            for (x = 0; x < LEVEL_W; ++x) {
                int32_t a = (x - 33) * (x - 33) + (y - 25) * (y - 25);
                int32_t b = (x - circle_x) * (x - circle_x) +
                            (y - circle_y) * (y - circle_y);
                if ((a <= 144) != (b <= 144))
                    pixel((uint8_t)x, (uint8_t)y, balanced ? 2U : 1U);
            }
        }
    } else {
        /* 法向量 -Y,-X；X=-1g时下白上黑，Y=+1g时左白右黑。 */
        for (y = 0; y < LEVEL_H; ++y) {
            for (x = 0; x < LEVEL_W; ++x) {
                float f = -yg * (x - 43.5f) - xg * (y - 19.5f);
                color = (f >= 0.0f) ? (balanced ? 2U : 1U) : 0U;
                pixel((uint8_t)x, (uint8_t)y, color);
            }
        }
    }
    lv_obj_invalidate(s_canvas);
}

/* 格式固定：X-1.00g / Y 0.00g / Z 0.00g；统一小数点列。 */
static void print_g(uint8_t n, char axis, float g)
{
    char *t = s_text[n];
    uint16_t num;
    if (g > 9.99f) g = 9.99f;
    if (g < -9.99f) g = -9.99f;
    t[0] = axis; t[1] = (g < -0.0001f) ? '-' : ' ';
    num = (uint16_t)(fabsf(g) * 100.0f + 0.5f);
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
    float x, y, z, angle, target;
    uint32_t seq;
    uint8_t m;
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

    /* Z与侧立之间有滞回；X/Y模式也加滞回防止45°抖动。 */
    if (s_mode == 2U) {
        if (fabsf(z) < 0.65f)
            s_mode = fabsf(y) > fabsf(x) ? 1U : 0U;
    } else if (fabsf(z) > 0.80f) {
        s_mode = 2U;
    } else if (s_mode == 0U && fabsf(y) > fabsf(x) + 0.12f) {
        s_mode = 1U;
    } else if (s_mode == 1U && fabsf(x) > fabsf(y) + 0.12f) {
        s_mode = 0U;
    }

    if (s_mode == 2U) {
        angle = atan2f(sqrtf(x*x + y*y), fabsf(z)) * s_to_deg;
    } else if (s_mode == 0U) {
        angle = atan2f(fabsf(y), fabsf(x)) * s_to_deg;
    } else {
        angle = atan2f(fabsf(x), fabsf(y)) * s_to_deg;
    }
    target = angle - s_zero_deg;
    if (target < 0.0f) target = -target;
    render_graphics(x, y, z, target);
    print_g(0U, 'X', x); print_g(1U, 'Y', y); print_g(2U, 'Z', z);
    print_angle(target);
    lv_obj_set_style_border_color(s_degree,
        lv_color_hex(target < 2.0f ? 0x00D060 : 0xFFFFFF), 0);
}

/* AUTO睡眠依旧有效：应用不设置PM_BLOCK。模式标志仅请求load_task换ODR；
 * PM suspend固定25Hz低功耗唤醒监控或ODR=0，PM resume恢复50Hz。 */
static void level_create(void)
{
    uint8_t i;
    if (s_screen) return;
    s_sample_valid = 0U; s_sample_seq = 0U; s_zero_deg = 0.0f;
    s_mode = 0U;
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
    lv_canvas_set_buffer(s_canvas, s_canvas_buf, LEVEL_W, LEVEL_H, LV_COLOR_FORMAT_I2);
    lv_obj_set_pos(s_canvas, 0, 0);
    palette_set(0U, 0, 0, 0);
    palette_set(1U, 255, 255, 255);
    palette_set(2U, 0, 208, 96);
    palette_set(3U, 100, 100, 100);

    /* 数字区固定黑底，左右图形不侵入；4×9px行高适配40px屏。 */
    for (i = 0U; i < 4U; ++i) {
        s_label[i] = lv_label_create(s_screen);
        lv_obj_set_pos(s_label[i], 96, 1 + 10 * i);
        lv_obj_set_size(s_label[i], 64, 9);
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
    lv_obj_set_pos(s_degree, 145, 31);
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
    if (action == APP_ACTION_UP) menu_app_exit();
    else if (action == APP_ACTION_ENTER) {
        /* CONF：以当前方向相对调零；MENU仍为退出。 */
        float x = s_ax*0.001f, y = s_ay*0.001f, z = s_az*0.001f;
        if (!s_sample_valid) return;
        if (s_mode == 2U) s_zero_deg = atan2f(sqrtf(x*x+y*y),fabsf(z))*s_to_deg;
        else if (s_mode == 0U) s_zero_deg = atan2f(fabsf(y),fabsf(x))*s_to_deg;
        else s_zero_deg = atan2f(fabsf(x),fabsf(y))*s_to_deg;
    }
}
static void level_destroy(void)
{
    SC7A20_AlgoLevelerSet(0U);
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
