/*
 * main_screen.c - compact 160x40 status screen.
 * Three fixed columns: BUS, battery, RTC/ETA. One 500ms UI refresh,
 * reading only existing device mirrors (no I2C in the UI thread).
 *
 * ============================ 主界面布局（手动修改前先看这里）============================
 * 面板 160x40，横向分三栏，纵向三行（行距 14px，见下方 MAIN_ROW_*）：
 *
 *   x:  0    2        52 62  64            103  114       160
 *       +----+---------+--+----+---------------+----+---------+
 *       |    |  BUS 栏 |箭|    |   电池栏       |    | 状态栏   |
 *       +----+---------+--+----+---------------+----+---------+
 *   行  |    端口状态(居中) |方向| BAT 功率       |  温度        |
 *   中  |    BUS 功率       | —  | BAT 电压       |  ETA         |
 *   低  |    BUS 电压       | —  | （图标 x64..103）|  时钟       |
 *
 * 各栏可用宽度：
 *   BUS 栏    x2..52 白色轮廓框（51x14，圆角 3，浅蓝填充），框内文字 x4 起
 *             端口行 48px 居中（最长 6 字符）；功率/电压 56px 左对齐
 *   方向箭头  x62..69，8px，固定不参与排版；只在充/放电时画 > 或 <
 *   电池栏    MAIN_BAT_X=68 起固定，图标 40x14（x68..107），
 *             功率/电压标签 x64 宽 48 = 6 字符，居中于图标上方
 *   状态栏    x114/x118 起，宽 44（温度，5 字符）/ 40（时钟、ETA）
 *
 * 【字体硬约束】主字体 lv_font_ter_u14b 是严格等宽：每字符 adv_w=128 即 8px，
 * line_height=14、base_line=2。所以：
 *     宽度(px) / 8 = 可容纳字符数      （56px=7、48px=6、44px=5、40px=5、32px=4）
 * 文字一律走 LV_LABEL_LONG_MODE_CLIP：超宽会被硬截断而不是换行；
 * 若出现"字被切掉"，要么减字符数，要么加宽标签并确认不会撞到相邻栏。
 *
 * 【垂直对齐】行盒 14px，但字形墨迹只占第 2..11 行（base_line=2）。
 * MAIN_ROW_* 是行盒顶边 y，不是墨迹顶边，所以调行位置时按 14 的倍数走最省事。
 *
 * 【颜色一律用 RGB565 码值】本机 LV_COLOR_DEPTH=16 且 display 格式为
 * LV_COLOR_FORMAT_RGB565_SWAPPED，24 位颜色最终会被量化到 RGB565，
 * 所以本文件所有颜色都写成"量化后的 24 位形式"，即每通道只保留高位：
 *     R,B 取高 5 位（须是 8 的倍数） / G 取高 6 位（须是 4 的倍数）
 * 即：代码里的 0xRRGGBB 值 == 屏幕真实显示值，不会再出现"写了一个屏幕
 * 根本显示不出的颜色"的错觉。当前用过的高亮色（括号内为量化前原值）：
 *     端口框底 0x60CCF8 (原 0x66CCFF)     A 口红 0xF85050 (原 0xFF5050)
 *     C 口黄   0xF0D040 (本来就是精确值)
 * 新增颜色请先量化再写入，别直接抄网页取色；recolor 字符串（#RRGGBB）
 * 走同一套量化，也要一起收敛。
 *
 * 常用改动速查：
 *   挪电池左右      → MAIN_BAT_X（电池栏所有对象都由它派生，改一处即可）
 *   挪整行上下      → MAIN_ROW_TOP / MID / BOT（注意 temp 额外 +1）
 *   改数值小数位    → main_refresh() 里的 mini_snprintf 格式串
 *   温度整数/小数   → update_clock_and_temp() 里的 t10 阈值
 *   电量条颜色阈值  → update_battery() 里的 pct 比较
 *   本文件只读设备镜像，不做 I2C；改数据来源请去驱动/算法层，别在这里加总线操作。
 * ==================================================================================
 */
#include "applications.h"
#include "lvgl.h"
#include "menu.h"
#include "functions.h"
#include "main_icons.h"
#include "menu_ui.h"
#include "mini_format.h"
#include <string.h>

/* ===================== 坐标与阈值常量（改布局基本只动这里） ===================== */
/* 三行行盒顶边 y（行距 14 = 字体 line_height，正好三行排满 40px 内屏）。
 * 期望视觉效果：三行等距，首行留 1px 上边距（墨迹从 y=1 开始）。 */
#define MAIN_ROW_TOP            (-1)
#define MAIN_ROW_MID            14
#define MAIN_ROW_BOT            28

/* 电池栏左边缘 x：电池图标与电量填充条、百分号标签全部由它派生。
 * 图标宽 40（x..x+39），左侧小凸起占 x+1..x+3，右侧填充条上限 35px + 凸起 3px。
 * 往左移要注意别压到方向箭头（箭头盒在 x62..69，实际墨迹居中于 x64..67）。 */
#define MAIN_BAT_X              65

/* A1 真实负载电流门限（mA）：A1 与 WLED 假插入共用，
 * 空载假插入不会有负载电流，故以电流区分真假插入，见 update_ports()。 */
#define MAIN_A1_LOAD_MA         20U

/* ETA 无效哨兵：显示 "--H--"。有效秒数限制为 <360000（最多99H59）。 */
#define MAIN_ETA_INVALID        0xFFFFFFFFUL

/* 每个信息标签的固定尺寸：宽由调用点给出，高恒为 14 = 一个行盒。
 * 高度必须 >= 字体 line_height，否则字形会被纵向裁切。 */
#define MAIN_LBL_H              14

/* 屏幕规格与刷新周期 */
#define MAIN_SCR_W              160
#define MAIN_SCR_H              40
#define MAIN_REFRESH_MS         500

/* 对象句柄：全部由 main_screen_create() 建立、main_screen_destroy() 置空。
 * 分三组，对应上面布局图的三栏： */
static lv_obj_t *s_scr;                                     /* 主界面根对象（160x40） */
static lv_obj_t *s_port, *s_bus_power, *s_bus_volt;         /* BUS 栏三行 */
static lv_obj_t *s_bat_power, *s_bat_volt, *s_dir;          /* 电池栏：功率/电压 + 方向箭头 */
static lv_obj_t *s_temp, *s_clock, *s_eta;                  /* 状态栏三行（ETA 在中行、时钟在低行） */
static lv_obj_t *s_bar, *s_tab, *s_bat_img, *s_pct;       /* 电池条/端部小条/实心图标/百分比 */
static lv_timer_t *s_timer;                                 /* 500ms 刷新定时器 */
/* 电量条动画：以 0.1% 为单位逐步逼近真实电量，避免数字跳变。
 * 充电时每 500ms 前进 16.7%，放电时直接对齐真实值。 */
static uint16_t s_bar_tenth;
/* ETA（剩余/充满时间）状态机。刻意用 IIR 平滑功率并限速刷新，
 * 否则瞬时电流会让倒计时每帧都在跳。 */
static uint32_t s_eta_power_mw;                             /* 平滑后的功率（mW） */
static uint32_t s_eta_bat_seconds = MAIN_ETA_INVALID;      /* 电池侧独立 ETA，秒 */
static uint32_t s_eta_bat_stamp;                           /* 电池ETA采样时刻，ms */
static uint32_t s_eta_seconds = MAIN_ETA_INVALID;          /* 显示ETA，秒 */
static uint8_t s_eta_mode, s_eta_age, s_eta_refresh;        /* 方向 / 稳定计数 / 刷新分频 */

/* 协议名缩写：仅用于主界面显示，驱动层的全名一律不动。
 * 注意 SW6306_ReadProtocol() 可能返回 NULL，所以第一行必须同时判空指针。
 * 新增协议时在这里补一行即可；未列出的协议原样显示（受标签宽度 CLIP 限制）。
 * 缩写后最长 4 字符（VOOC/SFCP/AFC/UFCS/UNK），加 "A:"/"C:" 前缀 = 6 字符。 */
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

/* 建立一个纯文字标签。所有主界面文字都必须经这里创建，以保证四条统一样式：
 *   - 无内边距：坐标 x 即文字起点，方便按上面的布局图直接算位置；
 *   - 主字体（等宽 u14b，8px/字符）；
 *   - CLIP 长模式：超宽硬截断，绝不换行（换行会破坏 40px 三行布局）；
 *   - 高度恒为 MAIN_LBL_H（14px = 一个行盒），否则字形被纵向裁切。
 * 参数 w 决定可容纳字符数 = w/8，改宽度前先数好文案字数。 */
static lv_obj_t *make_label(int32_t x, int32_t y, int32_t w, lv_text_align_t align)
{
    lv_obj_t *lbl = lv_label_create(s_scr);
    lv_obj_set_style_pad_all(lbl, 0, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_ter_u14b, 0);
    lv_obj_set_style_text_align(lbl, align, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_size(lbl, w, MAIN_LBL_H);
    lv_obj_set_pos(lbl, x, y);
    return lbl;
}

/* ---------- BUS 栏：端口状态（第一行） ----------
 * A1 与 WLED 的"假插入"计量通路共用：固件为了让 WLED 电流流经检流电阻进库仑计，
 * 会在开灯期间假插入 A1，此时 A1 通路是打开的但没有负载电流。
 * 所以判据是"通路打开 且 BUS 电流 > MAIN_A1_LOAD_MA"：
 *     通路开 + 无电流 = 假插入 → 不显示 A（避免用户以为插了东西）
 *     通路开 + 有电流 = 真插入 → 正常显示 A
 * C 口则是真实口，充电中或 C1 放电即视为活动。
 *
 * 字符串里的 #RRGGBB ... # 是 LVGL recolor 色码：A 红、C 黄，
 * 由 lv_label_set_recolor(s_port, true) 启用；BUS 两个数值标签也各自启用变色。
 * 色码本身不占显示宽度，但【要算进 buf 容量】。
 * 文案宽度上限 6 字符（48px），超出会被 CLIP 截断。 */
static void update_ports(void)
{
    static char buf[32];   /* 最长文案 "#F85050 A#+#F0D040 C#:5V" = 25 字符，留余量 */
    uint8_t a = SW6306_IsPortA1ON() && SW6306_ReadIBUS() > MAIN_A1_LOAD_MA;
    uint8_t c = SW6306_IsCharging() ||
                (SW6306_IsDischarging() && SW6306_IsPortC1ON());

    if (a && c) {
        /* 双口同时活动：不显示协议（单组 qcstat 无法分口） */
        mini_snprintf(buf, sizeof(buf), "#F85050 A#+#F8E040 C#:5V");
    } else if (a) {
        mini_snprintf(buf, sizeof(buf), "#F85050 A#:%s",
                      short_proto(SW6306_ReadProtocol()));
    } else if (c) {
        mini_snprintf(buf, sizeof(buf), "#F8E040 C#:%s",
                      short_proto(SW6306_ReadProtocol()));
    } else {
        mini_snprintf(buf, sizeof(buf), "OFF");
    }
    lv_label_set_text_static(s_port, buf);
    /* 蓝色底：A/C保留专属红黄；冒号、协议及+采用深灰。OFF纯黑。 */
    lv_obj_set_style_text_color(s_port,
        lv_color_hex((a || c) ? 0x384048 : 0x000000), 0);
}

/* ---------- ETA：电池估算与小电流倒计时取最小 ----------
 * 分离电池 ETA 的估计基准和当前显示值，避免小电流倒计时覆盖电池估算。
 * 电池 ETA 每 10s 更新一次，期间依靠 LVGL 毫秒 tick 递减显示秒数。 */
static void update_eta(uint32_t bat_uw)
{
    /* WLED独占且已确认假A1时，SW6306的放电标志可能仍为0。
     * 将其作为电池放电ETA来源；若真实充电正在进行，仍优先按充电计算。 */
    uint8_t mode = SW6306_IsCharging() ? 1U :
                   ((SW6306_IsDischarging() ||
                     (WLED_AlgoIsOn() && WLED_AlgoOwnsFakeA1())) ? 2U : 0U);
    uint32_t power_mw = (bat_uw + 500U) / 1000U;
    uint32_t now = lv_tick_get(), best = MAIN_ETA_INVALID;

    if (mode == 0U || power_mw < 200U) {
        s_eta_mode = mode;
        s_eta_age = s_eta_refresh = 0U;
        s_eta_bat_seconds = MAIN_ETA_INVALID;
    } else if (mode != s_eta_mode) {
        s_eta_mode = mode;
        s_eta_power_mw = power_mw;
        s_eta_age = s_eta_refresh = 0U;
        s_eta_bat_seconds = MAIN_ETA_INVALID;
    } else {
        if (power_mw > s_eta_power_mw)
            s_eta_power_mw += (power_mw - s_eta_power_mw + 7U) / 8U;
        else
            s_eta_power_mw -= (s_eta_power_mw - power_mw + 7U) / 8U;

        if (s_eta_age < 10U) ++s_eta_age;     /* 状态稳定5s后首次估计 */
        if (s_eta_age >= 10U && (s_eta_age == 10U || ++s_eta_refresh >= 20U)) {
            float full, rem, energy, sec;
            s_eta_age = 11U;
            s_eta_refresh = 0U;
            s_eta_bat_seconds = MAIN_ETA_INVALID;
            if (SW6306_CapacityLoadOK() && s_eta_power_mw >= 200U) {
                full = SW6306_ReadMaxEnergy_mWh();
                rem = SW6306_ReadRemainEnergy_mWh();
                if (full > 500.0f && rem >= 0.0f && rem <= full * 1.05f) {
                    energy = (mode == 1U) ? (full - rem) : rem;
                    sec = energy * 3600.0f / (float)s_eta_power_mw;
                    if (sec >= 0.0f && sec < 360000.0f)
                        s_eta_bat_seconds = (uint32_t)(sec + 0.5f);
                }
            }
            s_eta_bat_stamp = now;
        }
    }

    if (s_eta_bat_seconds != MAIN_ETA_INVALID) {
        uint32_t elapsed = (uint32_t)(now - s_eta_bat_stamp) / 1000U;
        best = elapsed < s_eta_bat_seconds ? s_eta_bat_seconds - elapsed : 0U;
    }
    /* LOW_CURRENT 即便没有足够的放电功率用于估算，仍显示超时倒计时。 */
    if (SW6306_AlgoGetSpecialMode() == SW6306_SPECIAL_LOW_CURRENT) {
        uint32_t low_ms = SW6306_AlgoGetLowCurrentRemainingMs();
        uint32_t low_seconds = (low_ms + 999U) / 1000U;
        if (best == MAIN_ETA_INVALID || low_seconds < best) best = low_seconds;
    }
    s_eta_seconds = best;
}

/* 电量填充和百分比：返回单层白字，保持原充电动画及低电闪烁。 */
static void update_battery(void)
{
    static char buf[8];
    uint8_t actual = SW6306_ReadCapacity();
    uint8_t pct;
    int32_t fill, body, tab;

    if (actual > 100U) actual = 100U;
    if (SW6306_IsCharging()) {
        s_bar_tenth = (uint16_t)(s_bar_tenth + 167U);
        if (s_bar_tenth >= (uint16_t)actual * 10U) s_bar_tenth = 0U;
    } else {
        s_bar_tenth = (uint16_t)actual * 10U;
    }
    pct = (uint8_t)(s_bar_tenth / 10U);
    fill = 37 * (int32_t)pct / 100;
    /* 主体保留 x=4..38 及上下、右侧 1px 边缘。
     * 端部在 y=5..8 覆盖内部接缝 x=3，与主体直接相连；
     * 端部外缘 x=0 不覆盖，原充电动画节奏不变。 */
    body = fill < 35 ? fill : 35;
    tab = fill > 35 ? fill - 35 : 0;

    /* Bar remains inside the body shape with one-pixel vertical inset. */
    lv_obj_set_size(s_bar, body, 12);
    lv_obj_set_pos(s_bar, MAIN_BAT_X + 39 - body, 1);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(
        (pct >= 30U) ? 0x00F008 : (pct >= 10U) ? 0xE8DC00 : 0xF80000), 0);
    if (tab > 0) {
        /* 端部宽度额外包含接缝 x=3，仅覆盖内部而不触及外轮廓。 */
        lv_obj_set_size(s_tab, tab + 1, 4);
        lv_obj_set_pos(s_tab, MAIN_BAT_X + 3 - tab, 5);
        lv_obj_remove_flag(s_tab, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_tab, LV_OBJ_FLAG_HIDDEN);
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

/* Dim the tens digit of each displayed time group, or the leading
 * tens digit of a battery measurement. The five/six visible characters
 * retain their original monospaced positions; LVGL recolor tags are hidden. */
static void set_dim_zeros(lv_obj_t *lbl, char *dst, uint32_t capacity,
                          const char *src, const char *dim_zero, uint8_t time_groups)
{
    uint32_t n = 0U;
    for (uint32_t i = 0U; src[i] != '\0'; ++i) {
        if (src[i] == '0' && (i == 0U || (time_groups && i == 3U))) {
            for (const char *p = dim_zero; *p != '\0' && n + 1U < capacity; ++p)
                dst[n++] = *p;
        } else if (n + 1U < capacity) {
            dst[n++] = src[i];
        }
    }
    dst[n] = '\0';
    lv_label_set_text_static(lbl, dst);
}

/* ---------- 状态栏：温度（第一行）、ETA（第二行）、时钟（第三行） ----------
 * 【温度格式】t10 = 温度 x10 的整数（0.1°C 分辨率），常态一律保留 1 位小数。
 * 退化阈值放到 ±1000°C（t10 的 ±10000）——那已远超 NTC 物理量程，
 * 等于"实际永远保留小数"；留这个分支只是防御显示缓冲被写爆。
 *   历史坑：曾把阈值写成 t10 >= 100（实际是 10.0°C）以及 1000（100.0°C），
 *   都会让常见温度提前丢掉小数（30.2°C 显示成 30°C）。宽度已从 44px 扩到
 *   52px，足够容纳 "-10.0°C"（7 字符）而不被 CLIP 截断。
 * 正负数的四舍五入：正数 +5 再除 10，负数取绝对值后同样处理再补负号，
 * 避免 C 的整除向零截断在负数上产生偏差。
 * 【时钟】用 RTC 秒的奇偶交替显示 "HH:MM" 与 "HH MM" 作为秒闪。
 * 月/时/分先做合法性校验，RTC 未初始化时显示 "--:--" 而不是乱码。
 * 【ETA】无效显示 --H--；低于1小时显示 mmMss（真实秒数），否则 hhHmm。 */
static void update_clock_and_temp(void)
{
    static char clock_buf[40], temp_buf[12], eta_buf[40];
    char plain[8];
    uint8_t low_current = SW6306_AlgoGetSpecialMode() == SW6306_SPECIAL_LOW_CURRENT;
    float temp = SW6306_ReadNTCTemp();
    int32_t t10 = (int32_t)(temp * 10.0f + ((temp >= 0.0f) ? 0.5f : -0.5f));

    /* 常态保留 0.1°C；±1000°C 以上才退化为整数（实际量程达不到，仅作防御） */
    if (t10 <= -10000 || t10 >= 10000) {
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
        mini_snprintf(plain, sizeof(plain),
                      (SD3078_ReadSec() & 1U) ? "%02u %02u" : "%02u:%02u",
                      (unsigned)SD3078_ReadHour(), (unsigned)SD3078_ReadMin());
    } else {
        mini_snprintf(plain, sizeof(plain), "--:--");
    }
    set_dim_zeros(s_clock, clock_buf, sizeof(clock_buf), plain,
                  "#889498 0#", 1U);

    if (s_eta_seconds == MAIN_ETA_INVALID) {
        mini_snprintf(plain, sizeof(plain), "--H--");
    } else if (s_eta_seconds < 3600U) {
        mini_snprintf(plain, sizeof(plain), "%02luM%02lu",
                      (unsigned long)(s_eta_seconds / 60U),
                      (unsigned long)(s_eta_seconds % 60U));
    } else {
        mini_snprintf(plain, sizeof(plain), "%02luH%02lu",
                      (unsigned long)(s_eta_seconds / 3600U),
                      (unsigned long)((s_eta_seconds % 3600U) / 60U));
    }
    lv_obj_set_style_text_color(s_eta,
        lv_color_hex(low_current ? 0xF880C8U : 0x88B4D8U), 0);
    set_dim_zeros(s_eta, eta_buf, sizeof(eta_buf), plain,
                  low_current ? "#A05480 0#" : "#587890 0#", 1U);
}

/* ---------- 整屏刷新（500ms，由 s_timer 驱动） ----------
 * 所有数值都从设备镜像读取，本函数不做任何 I2C。
 *
 * 【改数值精度就改这里的格式串】宽度已按 6 字符留足（48px）：
 *   BAT 功率  "%02lu.%02luW"  → "21.50W"   （0.01W）
 *   BAT 电压  "%02lu.%02luV" → "07.50V"  （0.01V，首位暗零）
 *   BUS 功率  "%02lu.%02luW"  → 0.01W
 *   BUS 电压  "%02lu.%02luV"  → 0.01V
 * 注意先算成"整数个最小单位"再拆整数/小数部分打印——mini_format 不支持 %f
 * （浮点格式化会显著增大固件体积），所以统一走整数运算 + 四舍五入。
 *
 * 【显示分辨率 ≠ 测量分辨率】BAT 电压显示到 0.01V（10mV），
 * 接近 SW6306 电池电压的 7mV/LSB 量化步距。
 *
 * 【总线静息判断】IBUS < 5mA 视为无负载，直接把 BUS 功率清零，
 * 避免小电流乘以电压后显示出无意义的零点几瓦。 */
/* 对齐原有 8px 字格，仅前导十位零降低同色系的亮度/饱和度。
 * 标签默认色仍代表当前工作模式（粉/绿/白），不影响其余字符。 */
static void set_bus_value(lv_obj_t *lbl, char *out, uint32_t size,
                          const char *plain, uint32_t dim_rgb)
{
    if (plain[0] == '0')
        mini_snprintf(out, size, "#%06lX 0#%s",
                      (unsigned long)dim_rgb, plain + 1);
    else
        mini_snprintf(out, size, "%s", plain);
    lv_label_set_text_static(lbl, out);
}

static void main_refresh(void)
{
    static char bus_pw[24], bus_v[24], bat_pw[24], bat_v[24], dir[2];
    char plain[12];
    /* uW = mV x mA；直接用 V x A 会引入浮点 */
    uint32_t bus_uw = (uint32_t)SW6306_ReadVBUS() * SW6306_ReadIBUS();
    uint32_t bat_uw = (uint32_t)SW6306_ReadVBAT() * SW6306_ReadIBAT();
    uint32_t centiw, cv;      /* centiw = 0.01W 为单位；cv = 电压最小单位 */
    sw6306_special_mode_t mode = SW6306_AlgoGetSpecialMode();
    uint32_t bus_rgb, dim_rgb;  /* BUS 主色与低饱和前导零，均RGB565量化 */

    update_ports();

    /* BUS 栏兼作"特殊模式"指示灯：小电流=粉、慢充=绿、正常=浅灰白。
     * 功率与电压共用同一个颜色，便于一眼看出"这一栏当前处于什么模式"。
     * dim_rgb 是前导十位零用的暗色 = 对应主色的【2/3 亮度】：
     * 主色逐通道 x2/3（保持色相）再量化到 RGB565，得到
     *   小电流 0xF880C8 -> 0xA05480    慢充 0x40DC80 -> 0x289050
     *   常态   0xF0F0F0 -> 0xA0A0A0
     * Rec.601 加权亮度比 dim/主色 = 65% / 65% / 67%，三档层次一致。
     * 历史调参：x2（过亮）→ 旧值 +25%（层次不稳）→ 1/2（过暗）→ 当前 2/3。 */
    bus_rgb = (mode == SW6306_SPECIAL_LOW_CURRENT) ? 0xF880C8U :
              (mode == SW6306_SPECIAL_SLOW_CHARGE) ? 0x40DC80U : 0xF0F0F0U;
    dim_rgb = (mode == SW6306_SPECIAL_LOW_CURRENT) ? 0xA05480U :
              (mode == SW6306_SPECIAL_SLOW_CHARGE) ? 0x289050U : 0xA0A0A0U;

    if (SW6306_ReadIBUS() < 5U) bus_uw = 0U;
    /* uW → 0.01W：+5000 再 /10000 即四舍五入 */
    centiw = (bus_uw + 5000U) / 10000U;
    if (centiw > 99999U) centiw = 99999U;     /* 钳位，防 5 位溢出打乱排版 */
    mini_snprintf(plain, sizeof(plain), "%02lu.%02luW",
                  (unsigned long)(centiw / 100U),
                  (unsigned long)(centiw % 100U));
    set_bus_value(s_bus_power, bus_pw, sizeof(bus_pw), plain, dim_rgb);
    lv_obj_set_style_text_color(s_bus_power, lv_color_hex(bus_rgb), 0);

    /* 电池功率：同样 0.01W 分辨率 */
    centiw = (bat_uw + 5000U) / 10000U;
    mini_snprintf(plain, sizeof(plain), "%02lu.%02luW",
                  (unsigned long)(centiw / 100U),
                  (unsigned long)(centiw % 100U));
    set_dim_zeros(s_bat_power, bat_pw, sizeof(bat_pw), plain,
                  "#586070 0#", 0U);

    /* BUS 电压 0.01V：mV → 10mV 单位，+5 再 /10 四舍五入 */
    cv = ((uint32_t)SW6306_ReadVBUS() + 5U) / 10U;
    mini_snprintf(plain, sizeof(plain), "%02lu.%02luV",
                  (unsigned long)(cv / 100U), (unsigned long)(cv % 100U));
    set_bus_value(s_bus_volt, bus_v, sizeof(bus_v), plain, dim_rgb);
    lv_obj_set_style_text_color(s_bus_volt, lv_color_hex(bus_rgb), 0);

    /* Battery voltage rounded to 0.01V, aligned with 00.00W above.
     * A 2S pack has no tens of volts: its "0" stays dark grey. */
    cv = ((uint32_t)SW6306_ReadVBAT() + 5U) / 10U;
    mini_snprintf(plain, sizeof(plain), "%02lu.%02luV",
                  (unsigned long)(cv / 100U), (unsigned long)(cv % 100U));
    set_dim_zeros(s_bat_volt, bat_v, sizeof(bat_v), plain,
                  "#586070 0#", 0U);

    /* 方向箭头：'>' 充电（流入电池）、'<' 放电；空闲时置空串即不画。
     * 标签位置固定，空串也不会影响其它元素排版。 */
    dir[0] = SW6306_IsCharging() ? '>' :
             (SW6306_IsDischarging() ? '<' : '\0');
    dir[1] = '\0';
    lv_label_set_text_static(s_dir, dir);
    lv_obj_set_style_text_color(s_dir, lv_color_hex(0xE8F0F0), 0);

    update_eta(bat_uw);
    update_clock_and_temp();
    update_battery();
    lv_obj_invalidate(s_scr); /* 整屏只失效一次，替代各子函数各刷一遍 */
}

/* 定时器回调：唯一入口就是 main_refresh()。
 * 周期由 create 里的 MAIN_REFRESH_MS 决定；改刷新率只需改那一处。 */
static void main_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!menu_ui_transition_busy()) main_refresh();
}

/* ---------- 对象创建（改布局主要看这里） ----------
 * 【创建顺序 = 图层顺序】后建的盖在上面。电池栏自下而上是：
 *     A8 实心图标 → 黑色百分比 → 电量条 → 端部小条
 * 即电量条盖住实心图标（否则实心图会把条挡掉）。
 *
 * 【改动清单——每一步都对应一个可见元素】
 *   1. 根对象 160x40 + 深色底
 *   2. BUS 栏浅蓝填充框（51x14 @ x2,y0，圆角 3，白描边）→ 端口状态标签在框内居中
 *   3. BUS 栏功率/电压两行
 *   4. 方向箭头（固定 x62..69，架在端口框与电池之间）
 *   5. 电池栏功率/电压两行
 *   6. 状态栏温度/ETA/时钟三行 + 各栏文字颜色
 *   7. 电池栏四层：A8 实心图标 → 黑色百分比 → 电量条 → 端部小条
 * 所有坐标都基于上面布局图的常量，尽量别再引入裸数字。 */


static void main_screen_create(void)
{
    lv_obj_t *frame;
    if (s_scr) return;                       /* 已创建则直接复用，防重复建树 */

    s_scr = lv_obj_create(NULL);
    lv_obj_set_size(s_scr, MAIN_SCR_W, MAIN_SCR_H);
    lv_obj_set_scrollbar_mode(s_scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x101010), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);

    /* 行盒 14px 而字形墨迹只占第 2..11 行，故 y=-1/13/27 对应墨迹
     * y=1..10 / 15..24 / 29..38：三行等距，首行留 1px 上边距。 */
    /* 端口框：浅蓝填充(0x66CCFF→RGB565 0x667F) + 白描边。
     * 注意 bg_opa 必须为 COVER 颜色才可见——原来是 TRANSP（透明底），
     * 只改 bg_color 不会有任何视觉变化。 */
    frame = lv_obj_create(s_scr);
    lv_obj_set_pos(frame, 3, 0);
    lv_obj_set_size(frame, 51, MAIN_LBL_H);
    lv_obj_set_style_radius(frame, 3, 0);
    lv_obj_set_style_bg_color(frame, lv_color_hex(0x70E0E8), 0);
    lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(frame, 0, 0);
    lv_obj_remove_flag(frame, LV_OBJ_FLAG_SCROLLABLE);
    /* BUS 栏三行：左对齐 x5、宽 56 = 7 字符（够 "99.99W" 这类最长格式）。
     * 端口行单独居中：端口文案最长 6 字符 = 48px（协议名经 short_proto 缩写后
     * 最多 4 字符，加 "A:"/"C:" 前缀）。 */
    s_port      = make_label(5,   MAIN_ROW_TOP + 1, 48, LV_TEXT_ALIGN_CENTER);
    s_bus_power = make_label(5,   MAIN_ROW_MID, 56, LV_TEXT_ALIGN_LEFT);
    s_bus_volt  = make_label(5,   MAIN_ROW_BOT, 56, LV_TEXT_ALIGN_LEFT);

    /* 8px 箭头位于端口框右缘与电池图标左缘之间，盒 x55..62、墨迹居中于 x57..60；
     * 空闲时不画箭头（空串）但保留对象，避免影响其它元素排版。 */
    s_dir       = make_label(55,  MAIN_ROW_TOP, 8, LV_TEXT_ALIGN_CENTER);
    /* 电池栏两行：x62 起、宽 48 = 6 字符，居中于电池图标（x68..107）上方。
     * 6 字符正好容纳 "21.50W" / "07.50V" 这类对齐格式。
     * 文字实际只占约 x71..104（居中），与箭头墨迹(x57..60)不相撞；
     * 若以后把文案改成左对齐或加长到 6 字符以上，需重新核对。 */
    s_bat_power = make_label(62,  MAIN_ROW_MID, 48, LV_TEXT_ALIGN_CENTER);
    s_bat_volt  = make_label(62,  MAIN_ROW_BOT, 48, LV_TEXT_ALIGN_CENTER);

    /* 状态栏：温度 52px、ETA/时钟 40px。
     * 温度行 +1，借用顶行余量把字形下移 1px 视觉居中；宽度扩到 52px 是为了
     * 让 "-10.0°C"（7 字符）也有余量，从而常态一直保留 1 位小数。
     * 行序：温度(上) / ETA(中) / 时钟(下)——刻意把 ETA 放中间、
     * 时钟放低行，读时间时视线更靠下。 */
    s_temp      = make_label(106, MAIN_ROW_TOP + 1, 52, LV_TEXT_ALIGN_RIGHT);
    s_eta       = make_label(118, MAIN_ROW_MID, 40, LV_TEXT_ALIGN_RIGHT);
    s_clock     = make_label(118, MAIN_ROW_BOT, 40, LV_TEXT_ALIGN_RIGHT);
    /* 配色分层：温度/时钟（主信息）最亮，ETA 次之带蓝调，
     * BAT 功率/电压压暗一档——让时间与温度成为视觉重点。
     * BAT 标签在 0xB8C4D0 基础上再降 10% 亮度（Rec.601 194 -> 175），
     * 取值 0xA8B0C0 是 RGB565 色域内最接近的一档。 */
    lv_obj_set_style_text_color(s_bat_power, lv_color_hex(0xA8B0C0), 0);
    lv_obj_set_style_text_color(s_bat_volt,  lv_color_hex(0xA8B0C0), 0);
    lv_obj_set_style_text_color(s_temp,      lv_color_hex(0xD0DCE0), 0);
    lv_obj_set_style_text_color(s_clock,     lv_color_hex(0xD0DCE0), 0);
    lv_obj_set_style_text_color(s_eta,       lv_color_hex(0x88B4D8), 0);
    lv_label_set_recolor(s_port, true);      /* A/C端口着色 */
    lv_label_set_recolor(s_bus_power, true); /* BUS十位前导0低饱和 */
    lv_label_set_recolor(s_bus_volt, true);
    lv_label_set_recolor(s_bat_power, true);
    lv_label_set_recolor(s_bat_volt, true);
    lv_label_set_recolor(s_clock, true);
    lv_label_set_recolor(s_eta, true);

    /* ---------- 电池栏图层顺序（后建者在上）----------
     *   先建 实心图标 s_bat_img
     *   再建 电量条 s_bar / 端部条 s_tab
     *   最后 百分号 s_pct      ← 必须最后建，否则会被电量条盖住
     * 这样自上而下是：百分号 > 电量条 > 实心图标。
     * 【注意】s_pct 特意从 s_bat_img 子对象解绑：若仍作子对象，它会随图标
     * 一起被电量条压住而看不见。代价是它不再自动跟随图标移动，位置改为
     * 按 MAIN_BAT_X 手工对齐。 */

    /* 电池实心图标（40x14 A8） */
    s_bat_img = lv_image_create(s_scr);
    lv_image_set_src(s_bat_img, &main_icon_battery);
    lv_obj_set_style_image_recolor(s_bat_img, lv_color_hex(0xD0DCE0), 0);
    lv_obj_set_style_image_recolor_opa(s_bat_img, LV_OPA_COVER, 0);
    lv_obj_set_pos(s_bat_img, MAIN_BAT_X, 0);

    /* 电量填充条：主体 x=4..38、y=1..12，保留右侧和上下 1px 图标边框；
     * 实际尺寸每次刷新由 update_battery() 设定。 */
    s_bar = lv_obj_create(s_scr);
    lv_obj_set_style_border_width(s_bar, 0, 0);
    lv_obj_set_style_radius(s_bar, 0, 0);
    lv_obj_set_style_pad_all(s_bar, 0, 0);
    lv_obj_remove_flag(s_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_bar, 0, 12);
    lv_obj_set_pos(s_bar, MAIN_BAT_X + 39, 1);

    /* 端部在填满主体后才出现，最终覆盖 x=1..3、y=5..8，并连接主体。 */
    s_tab = lv_obj_create(s_scr);
    lv_obj_set_style_border_width(s_tab, 0, 0);
    lv_obj_set_style_radius(s_tab, 0, 0);
    lv_obj_set_style_pad_all(s_tab, 0, 0);
    lv_obj_remove_flag(s_tab, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_tab, 0, 4);
    lv_obj_set_pos(s_tab, MAIN_BAT_X + 3, 5);
    lv_obj_set_style_bg_color(s_tab, lv_color_hex(0x00F008), 0);
    lv_obj_add_flag(s_tab, LV_OBJ_FLAG_HIDDEN);   /* 默认隐藏，由刷新逻辑按需显示 */

    /* 百分号：最后创建 => 位于最上层，不会被电量条盖住。
     * 独立对象，水平居中于图标主体（宽 40）。 */
    s_pct = lv_label_create(s_scr);
    lv_obj_set_style_pad_all(s_pct, 0, 0);
    lv_obj_set_style_text_font(s_pct, &lv_font_ter_u14b, 0);
    lv_obj_set_style_text_color(s_pct, lv_color_hex(0x000000), 0);
    lv_obj_set_size(s_pct, 40, MAIN_LBL_H);
    lv_obj_set_pos(s_pct, MAIN_BAT_X + 1, 0);
    lv_obj_set_style_text_align(s_pct, LV_TEXT_ALIGN_CENTER, 0);

    /* ETA 状态机复位：首帧必然显示无效，等稳定 5s 后才有值 */
    s_eta_mode = 0U;
    s_eta_bat_seconds = s_eta_seconds = MAIN_ETA_INVALID;
    s_eta_age = s_eta_refresh = 0U;
    main_refresh();                                   /* 立即画一帧，避免空白 */
    s_timer = lv_timer_create(main_timer_cb, MAIN_REFRESH_MS, NULL);
    lv_obj_invalidate(s_scr);
}

/* 重新进入主界面：只切换显示，不重建对象（省内存与时间）。
 * 对象树在 create 时建一次，destroy 时才真正释放。 */
static void main_screen_activate(void)
{
    if (s_scr) {
        lv_screen_load(s_scr);
        lv_obj_invalidate(s_scr);
    }
}

/* 彻底销毁：必须先删定时器再删根对象，
 * 否则定时器回调会在对象已释放后访问 s_* 句柄（use-after-free）。
 * 末尾把所有句柄置 NULL，与 create 的 s_scr 判空配合，允许再次 enter。 */
static void main_screen_destroy(void)
{
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    if (s_scr) { lv_obj_delete(s_scr); s_scr = NULL; }
    s_port = s_bus_power = s_bus_volt = NULL;
    s_bat_power = s_bat_volt = s_dir = NULL;
    s_temp = s_clock = s_eta = NULL;
    s_bar = s_tab = s_bat_img = s_pct = NULL;
}

/* ---------- 主界面按键语义 ----------
 * CONF(LED) 单击开A1、双击开关WLED、WLED关闭时长按关闭A1；
 * WLED开启时长按持续调光，松手换方向。
 * NEXT(PWR) 单击触发C1插入、双击切换小电流/慢充、长按关闭C1。
 * MENU单击进入菜单。端口命令由对应任务执行，UI不直接访问I2C。 */
void main_screen_run(app_action_t action)
{
    static int8_t s_wled_dir = -1;  /* -1 减亮 / +1 增亮 */
    static uint8_t s_dim_div;
    static uint8_t s_led_hold_mode; /* 0=未长按 1=调光 2=关闭A1已执行 */

    switch (action) {
    case APP_ACTION_UP:
        menu_open_from_main();
        break;
    case APP_ACTION_ENTER:
        WLED_AlgoRequestA1On();
        break;
    case APP_ACTION_ENTER_DBL:
        WLED_AlgoRequestToggle();
        break;
    case APP_ACTION_ENTER_HOLD:
        if (s_led_hold_mode == 0U) {
            s_led_hold_mode = WLED_AlgoIsOn() ? 1U : 2U;
            if (s_led_hold_mode == 2U) WLED_AlgoRequestA1Off();
        }
        if (s_led_hold_mode == 1U && ++s_dim_div >= 8U) {
            s_dim_div = 0U;
            WLED_AlgoRequestBrightnessStep(s_wled_dir);
        }
        break;
    case APP_ACTION_ENTER_HOLD_END:
        if (s_led_hold_mode == 1U) s_wled_dir = (int8_t)(-s_wled_dir);
        s_dim_div = 0U;
        s_led_hold_mode = 0U;
        break;
    case APP_ACTION_DOWN:
        SW6306_AlgoRequestC1Insert();
        break;
    case APP_ACTION_DOWN_DBL:
        SW6306_AlgoRequestSpecialToggle();
        break;
    case APP_ACTION_DOWN_HOLD:
        SW6306_AlgoRequestC1Remove();
        break;
    default:
        break;
    }
}

/* 注册为主界面应用：menu.c 统一管理生命周期（enter 时 create+activate，
 * 离开时 destroy），run 即上面的按键处理入口。 */
const menu_app_t menu_app_main = {
    .label    = "Main",
    .create   = main_screen_create,
    .activate = main_screen_activate,
    .run      = main_screen_run,
    .destroy  = main_screen_destroy,
};
