/*
 * main_screen.c - compact 160x40 status screen.
 * Three fixed columns: BUS, battery, RTC/ETA. One 500ms UI refresh,
 * reading only existing device mirrors (no I2C in the UI thread).
 */
#include "applications.h"
#include "lvgl.h"
#include "menu.h"
#include "functions.h"
#include "main_icons.h"
#include "mini_format.h"
#include <string.h>

#define MAIN_BAT_X              72
#define MAIN_ROW_TOP            (-1)
#define MAIN_ROW_MID            13
#define MAIN_ROW_BOT            27
#define MAIN_A1_LOAD_MA         20U
#define MAIN_ETA_INVALID        0xFFFFU

static lv_obj_t *s_scr;
static lv_obj_t *s_port, *s_bus_power, *s_bus_volt;
static lv_obj_t *s_bat_power, *s_bat_volt, *s_dir;
static lv_obj_t *s_temp, *s_clock, *s_eta;
static lv_obj_t *s_bar, *s_tab, *s_bat_img, *s_pct;
static lv_timer_t *s_timer;
static uint16_t s_bar_tenth;
static uint32_t s_eta_power_mw;
static uint16_t s_eta_minutes;
static uint8_t s_eta_mode, s_eta_age, s_eta_refresh;

/* UI-only protocol abbreviation; SW6306 driver's full names stay unchanged. */
static const char *short_proto(const char *p)
{
    if (!p || !strcmp(p, "NONE")) return "5V";
    if (!strcmp(p, "PDFIX")) return "PD";
    if (!strcmp(p, "PDPPS")) return "PPS";
    if (!strcmp(p, "QC2.0")) return "QC2";
    if (!strcmp(p, "QC3.0")) return "QC3";
    if (!strcmp(p, "PE1.1")) return "PE1";
    if (!strcmp(p, "PE2.0")) return "PE2";
    if (!strcmp(p, "VOOC1")) return "VOOC";
    return p;
}

static lv_obj_t *make_label(int32_t x, int32_t y, int32_t w, lv_text_align_t align)
{
    lv_obj_t *lbl = lv_label_create(s_scr);
    lv_obj_set_style_pad_all(lbl, 0, 0);
    lv_obj_set_style_text_font(lbl, menu_font_main(), 0);
    lv_obj_set_style_text_align(lbl, align, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_size(lbl, w, 14);
    lv_obj_set_pos(lbl, x, y);
    return lbl;
}

/* A1 is shared with WLED's fake-insert metering path; zero BUS current
 * means the fake A1 must not be advertised as a real output port. */
static void update_ports(void)
{
    static char buf[48];
    uint8_t a = SW6306_IsPortA1ON() && SW6306_ReadIBUS() > MAIN_A1_LOAD_MA;
    uint8_t c = SW6306_IsCharging() ||
                (SW6306_IsDischarging() && SW6306_IsPortC1ON());

    if (a && c) {
        mini_snprintf(buf, sizeof(buf), "#FF5050 A#+#F0D040 C#:5V");
    } else if (a) {
        mini_snprintf(buf, sizeof(buf), "#FF5050 A#:%s",
                      short_proto(SW6306_ReadProtocol()));
    } else if (c) {
        mini_snprintf(buf, sizeof(buf), "#F0D040 C#:%s",
                      short_proto(SW6306_ReadProtocol()));
    } else {
        mini_snprintf(buf, sizeof(buf), "OFF");
    }
    lv_label_set_text_static(s_port, buf);
    lv_obj_set_style_text_color(s_port, (a || c) ?
                                lv_color_white() : lv_color_hex(0x8A93A0), 0);
}

static void update_eta(uint32_t bat_uw)
{
    uint8_t mode = SW6306_IsCharging() ? 1U :
                   (SW6306_IsDischarging() ? 2U : 0U);
    uint32_t power_mw = (bat_uw + 500U) / 1000U;
    float full, rem, energy, minutes;

    if (mode == 0U || power_mw < 200U) {
        s_eta_mode = mode;
        s_eta_age = 0U;
        s_eta_minutes = MAIN_ETA_INVALID;
        return;
    }
    if (mode != s_eta_mode) {
        s_eta_mode = mode;
        s_eta_power_mw = power_mw;
        s_eta_age = 0U;
        s_eta_refresh = 0U;
        s_eta_minutes = MAIN_ETA_INVALID;
        return;
    }

    /* 500ms IIR (~4s time constant). Do not use instantaneous current
     * for an ETA that otherwise changes visibly every frame. */
    if (power_mw > s_eta_power_mw)
        s_eta_power_mw += (power_mw - s_eta_power_mw + 7U) / 8U;
    else
        s_eta_power_mw -= (s_eta_power_mw - power_mw + 7U) / 8U;

    if (s_eta_age < 10U) {
        ++s_eta_age;                /* 5s settling after a direction change */
        if (s_eta_age < 10U) return;
    } else if (++s_eta_refresh < 20U) {
        return;                      /* Update ETA every 10s */
    }
    s_eta_refresh = 0U;

    if (!SW6306_CapacityLoadOK() || s_eta_power_mw < 200U) {
        s_eta_minutes = MAIN_ETA_INVALID;
        return;
    }
    full = SW6306_ReadMaxEnergy_mWh();
    rem = SW6306_ReadRemainEnergy_mWh();
    if (!(full > 500.0f && rem >= 0.0f && rem <= full * 1.05f)) {
        s_eta_minutes = MAIN_ETA_INVALID;
        return;
    }
    energy = (mode == 1U) ? (full - rem) : rem;
    minutes = energy * 60.0f / (float)s_eta_power_mw;
    s_eta_minutes = (minutes >= 0.0f && minutes < 6000.0f) ?
                     (uint16_t)(minutes + 0.5f) : MAIN_ETA_INVALID;
}

static void update_battery(void)
{
    static char buf[8];
    uint8_t actual = SW6306_ReadCapacity();
    uint8_t pct;

    if (actual > 100U) actual = 100U;
    if (SW6306_IsCharging()) {
        s_bar_tenth = (uint16_t)(s_bar_tenth + 167U);
        if (s_bar_tenth >= (uint16_t)actual * 10U) s_bar_tenth = 0U;
    } else {
        s_bar_tenth = (uint16_t)actual * 10U;
    }
    pct = (uint8_t)(s_bar_tenth / 10U);

    /* Uploaded outline 40x14: body x=4..38 / y=1..12;
     * left terminal x=1..3 / y=4..9. Draw fill behind the A8 outline. */
    {
        int32_t fill = (38 * (int32_t)pct) / 100;
        int32_t body = (fill < 35) ? fill : 35;
        int32_t tab = (fill > 35) ? (fill - 35) : 0;

        lv_obj_set_size(s_bar, body, 12);
        lv_obj_set_pos(s_bar, MAIN_BAT_X + 4 + 35 - body, 1);
        lv_obj_set_style_bg_color(s_bar, lv_color_hex(
            (pct >= 30U) ? 0x00F00F : (pct >= 10U) ? 0xEFDF00 : 0xFF0000), 0);
        if (tab > 0) {
            lv_obj_set_size(s_tab, tab, 6);
            lv_obj_set_pos(s_tab, MAIN_BAT_X + 1 + 3 - tab, 4);
            lv_obj_remove_flag(s_tab, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_tab, LV_OBJ_FLAG_HIDDEN);
        }
    }

    mini_snprintf(buf, sizeof(buf), "%u%%", (unsigned)actual);
    lv_label_set_text_static(s_pct, buf);
    if (actual <= 5U && SW6306_IsDischarging() &&
        SD3078_ReadMonth() >= 1U && SD3078_ReadMonth() <= 12U &&
        !(SD3078_ReadSec() & 1U)) {
        lv_obj_add_flag(s_pct, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_pct, LV_OBJ_FLAG_HIDDEN);
    }
}

static void update_clock_and_temp(void)
{
    static char clock_buf[8], temp_buf[12], eta_buf[8];
    float temp = SW6306_ReadNTCTemp();
    int32_t t10 = (int32_t)(temp * 10.0f + ((temp >= 0.0f) ? 0.5f : -0.5f));

    /* Rounded -10.0C and below omit decimals; all strings fit 44px.
     * Degree glyph extends above ordinary capital/digit pixels; the
     * temperature glyph shares the fixed top row. */
    if (t10 <= -100 || t10 >= 100) {
        int32_t integer = (t10 >= 0) ? (t10 + 5) / 10 : -((-t10 + 5) / 10);
        mini_snprintf(temp_buf, sizeof(temp_buf), "%ld°C", (long)integer);
    } else {
        uint32_t mag = (uint32_t)((t10 < 0) ? -t10 : t10);
        mini_snprintf(temp_buf, sizeof(temp_buf), "%s%lu.%lu°C",
                      (t10 < 0) ? "-" : "",
                      (unsigned long)(mag / 10U),
                      (unsigned long)(mag % 10U));
    }
    lv_label_set_text_static(s_temp, temp_buf);

    if (SD3078_ReadMonth() >= 1U && SD3078_ReadMonth() <= 12U &&
        SD3078_ReadHour() <= 23U && SD3078_ReadMin() <= 59U) {
        mini_snprintf(clock_buf, sizeof(clock_buf),
                      (SD3078_ReadSec() & 1U) ? "%02u %02u" : "%02u:%02u",
                      (unsigned)SD3078_ReadHour(), (unsigned)SD3078_ReadMin());
    } else {
        mini_snprintf(clock_buf, sizeof(clock_buf), "--:--");
    }
    lv_label_set_text_static(s_clock, clock_buf);

    if (s_eta_minutes == MAIN_ETA_INVALID) {
        lv_label_set_text_static(s_eta, "--H--");
    } else {
        mini_snprintf(eta_buf, sizeof(eta_buf), "%02luH%02lu",
                      (unsigned long)(s_eta_minutes / 60U),
                      (unsigned long)(s_eta_minutes % 60U));
        lv_label_set_text_static(s_eta, eta_buf);
    }
}

static void main_refresh(void)
{
    static char bus_pw[12], bat_pw[12], bus_v[12], bat_v[12], dir[2];
    uint32_t bus_uw = (uint32_t)SW6306_ReadVBUS() * SW6306_ReadIBUS();
    uint32_t bat_uw = (uint32_t)SW6306_ReadVBAT() * SW6306_ReadIBAT();
    uint32_t centiw, deciw, cv;
    sw6306_special_mode_t mode = SW6306_AlgoGetSpecialMode();

    update_ports();

    if (SW6306_ReadIBUS() < 5U) bus_uw = 0U;
    centiw = (bus_uw + 5000U) / 10000U;
    if (centiw > 99999U) centiw = 99999U;
    mini_snprintf(bus_pw, sizeof(bus_pw), "%02lu.%02luW",
                  (unsigned long)(centiw / 100U),
                  (unsigned long)(centiw % 100U));
    lv_label_set_text_static(s_bus_power, bus_pw);
    lv_obj_set_style_text_color(s_bus_power, lv_color_hex(
        (mode == SW6306_SPECIAL_LOW_CURRENT) ? 0xFF80CC :
        (mode == SW6306_SPECIAL_SLOW_CHARGE) ? 0x45DD85 : 0xF0F2F5), 0);

    deciw = (bat_uw + 50000U) / 100000U;
    if (deciw > 999U) deciw = 999U; /* Preserve five-character battery slot. */
    mini_snprintf(bat_pw, sizeof(bat_pw), "%02lu.%luW",
                  (unsigned long)(deciw / 10U),
                  (unsigned long)(deciw % 10U));
    lv_label_set_text_static(s_bat_power, bat_pw);

    cv = ((uint32_t)SW6306_ReadVBUS() + 5U) / 10U;
    mini_snprintf(bus_v, sizeof(bus_v), "%02lu.%02luV",
                  (unsigned long)(cv / 100U), (unsigned long)(cv % 100U));
    lv_label_set_text_static(s_bus_volt, bus_v);

    cv = ((uint32_t)SW6306_ReadVBAT() + 5U) / 10U;
    mini_snprintf(bat_v, sizeof(bat_v), "%lu.%02luV",
                  (unsigned long)(cv / 100U), (unsigned long)(cv % 100U));
    lv_label_set_text_static(s_bat_volt, bat_v);

    dir[0] = SW6306_IsCharging() ? '>' :
             (SW6306_IsDischarging() ? '<' : '-');
    dir[1] = '\0';
    lv_label_set_text_static(s_dir, dir);
    lv_obj_set_style_text_color(s_dir, lv_color_hex(
        (dir[0] == '-') ? 0x8A93A0 : 0xEDF1F7), 0);

    update_eta(bat_uw);
    update_clock_and_temp();
    update_battery();
    lv_obj_invalidate(s_scr); /* Once per frame instead of four full invalidates. */
}

static void main_timer_cb(lv_timer_t *t)
{
    (void)t;
    main_refresh();
}

static void main_screen_create(void)
{
    if (s_scr) return;

    s_scr = lv_obj_create(NULL);
    lv_obj_set_size(s_scr, 160, 40);
    lv_obj_set_scrollbar_mode(s_scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x101010), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);

    /* Actual u14b digit glyphs use rows 2..11 of a 14px logical line:
     * y=-1/13/27 => visible y=1..10/15..24/29..38. */
    s_port      = make_label(2,   MAIN_ROW_TOP, 56, LV_TEXT_ALIGN_LEFT);
    s_bus_power = make_label(2,   MAIN_ROW_MID, 56, LV_TEXT_ALIGN_LEFT);
    s_bus_volt  = make_label(2,   MAIN_ROW_BOT, 56, LV_TEXT_ALIGN_LEFT);

    s_dir       = make_label(64,  MAIN_ROW_TOP, 8, LV_TEXT_ALIGN_CENTER);
    s_bat_power = make_label(72,  MAIN_ROW_MID, 40, LV_TEXT_ALIGN_CENTER);
    s_bat_volt  = make_label(72,  MAIN_ROW_BOT, 40, LV_TEXT_ALIGN_CENTER);

    /* Use the common top-row baseline; degree uses the spare margin. */
    s_temp      = make_label(114, MAIN_ROW_TOP, 44, LV_TEXT_ALIGN_RIGHT);
    s_clock     = make_label(118, MAIN_ROW_MID, 40, LV_TEXT_ALIGN_RIGHT);
    s_eta       = make_label(118, MAIN_ROW_BOT, 40, LV_TEXT_ALIGN_RIGHT);
    lv_obj_set_style_text_color(s_bat_power, lv_color_hex(0xD6DCE3), 0);
    lv_obj_set_style_text_color(s_bat_volt,  lv_color_hex(0xD6DCE3), 0);
    lv_obj_set_style_text_color(s_temp,      lv_color_hex(0xBCC6D0), 0);
    lv_obj_set_style_text_color(s_clock,     lv_color_hex(0xBCC6D0), 0);
    lv_obj_set_style_text_color(s_eta,       lv_color_hex(0x8CB7DC), 0);
    lv_label_set_recolor(s_port, true);

    /* Battery fill is behind the uploaded 40x14 A8 outline. */
    s_bar = lv_obj_create(s_scr);
    lv_obj_set_style_border_width(s_bar, 0, 0);
    lv_obj_set_style_radius(s_bar, 0, 0);
    lv_obj_set_style_pad_all(s_bar, 0, 0);
    lv_obj_remove_flag(s_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_bar, 0, 12);
    lv_obj_set_pos(s_bar, MAIN_BAT_X + 39, 1);

    s_tab = lv_obj_create(s_scr);
    lv_obj_set_style_border_width(s_tab, 0, 0);
    lv_obj_set_style_radius(s_tab, 0, 0);
    lv_obj_set_style_pad_all(s_tab, 0, 0);
    lv_obj_remove_flag(s_tab, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_tab, 0, 6);
    lv_obj_set_pos(s_tab, MAIN_BAT_X + 3, 4);
    lv_obj_set_style_bg_color(s_tab, lv_color_hex(0x00F00F), 0);
    lv_obj_add_flag(s_tab, LV_OBJ_FLAG_HIDDEN);

    s_bat_img = lv_image_create(s_scr);
    lv_image_set_src(s_bat_img, &main_icon_battery);
    lv_obj_set_style_image_recolor(s_bat_img, lv_color_white(), 0);
    lv_obj_set_style_image_recolor_opa(s_bat_img, LV_OPA_COVER, 0);
    lv_obj_set_pos(s_bat_img, MAIN_BAT_X, 0);

    s_pct = lv_label_create(s_bat_img);
    lv_obj_set_style_pad_all(s_pct, 0, 0);
    lv_obj_set_style_text_font(s_pct, menu_font_main(), 0);
    lv_obj_set_style_text_color(s_pct, lv_color_white(), 0);
    lv_obj_align(s_pct, LV_ALIGN_CENTER, 1, 0);

    s_eta_mode = 0U;
    s_eta_minutes = MAIN_ETA_INVALID;
    s_eta_age = s_eta_refresh = 0U;
    main_refresh();
    s_timer = lv_timer_create(main_timer_cb, 500, NULL);
    lv_obj_invalidate(s_scr);
}

static void main_screen_activate(void)
{
    if (s_scr) {
        lv_screen_load(s_scr);
        lv_obj_invalidate(s_scr);
    }
}

static void main_screen_destroy(void)
{
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    if (s_scr) { lv_obj_delete(s_scr); s_scr = NULL; }
    s_port = s_bus_power = s_bus_volt = NULL;
    s_bat_power = s_bat_volt = s_dir = NULL;
    s_temp = s_clock = s_eta = NULL;
    s_bar = s_tab = s_bat_img = s_pct = NULL;
}

/* Buttons: MENU opens menu, CONF double-click toggles WLED,
 * CONF hold adjusts WLED brightness, NEXT double-click toggles
 * low-current/slow-charge according to SW6306's charging state. */
void main_screen_run(app_action_t action)
{
    static int8_t s_wled_dir = -1;
    static uint8_t s_dim_div = 0U;

    switch (action) {
    case APP_ACTION_UP:
        main_screen_destroy();
        menu_open_from_main();
        break;
    case APP_ACTION_ENTER_DBL:
        WLED_AlgoRequestToggle();
        break;
    case APP_ACTION_ENTER_HOLD:
        if (++s_dim_div >= 8U) {
            s_dim_div = 0U;
            WLED_AlgoRequestBrightnessStep(s_wled_dir);
        }
        break;
    case APP_ACTION_ENTER_HOLD_END:
        s_wled_dir = (int8_t)(-s_wled_dir);
        break;
    case APP_ACTION_DOWN_DBL:
        SW6306_AlgoRequestSpecialToggle();
        break;
    default:
        break;
    }
}

const menu_app_t menu_app_main = {
    .label    = "Main",
    .create   = main_screen_create,
    .activate = main_screen_activate,
    .run      = main_screen_run,
    .destroy  = main_screen_destroy,
};
