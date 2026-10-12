#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Regenerate the 40x14 solid main-screen battery icon (A8 for LVGL 9).

几何定义（与 firmware/Applications/functions/main_icons.c 保持一致）：
    实心填充，每行是一个闭区间 [left, right]：
        y0 [4,38]   y1..y3  [3,39]
        y4 [1,39]   y5..y8  [0,39]      <- 左端部在此凸出
        y9 [1,39]   y10..y12 [3,39]
        y13 [4,38]
    其中 (0,5) 与 (0,8) 被置透明，使端部实际凸出部分为 y6..y7
    （即相对 y5..y8 的完整凸出，上下界各收 1px）。

注意：本脚本曾定义"1px 空心轮廓"，与后来手改的实心图标分叉；现改为按实心
几何生成，避免再跑一次就把实心图标覆盖回轮廓。方向箭头仍用 u14b 字体，
不是位图资源。
"""
from pathlib import Path

OUT_DIR = Path(__file__).resolve().parent.parent

W, H = 40, 14

# 每行实心填充区间 [left, right]（闭区间）；或单个像素的透明挖除
ROWS = {
    0:  (4, 38),
    1:  (3, 39),
    2:  (3, 39),
    3:  (3, 39),
    4:  (1, 39),
    5:  (0, 39),
    6:  (0, 39),
    7:  (0, 39),
    8:  (0, 39),
    9:  (1, 39),
    10: (3, 39),
    11: (3, 39),
    12: (3, 39),
    13: (4, 38),
}

# 端部上下界各收 1px：把这两个像素挖空
CLEAR = ((0, 5), (0, 8))


def battery_pixel(x, y):
    if (x, y) in CLEAR:
        return 0
    lo, hi = ROWS[y]
    return 255 if lo <= x <= hi else 0


def render_data():
    cells = []
    for y in range(H):
        row = [battery_pixel(x, y) for x in range(W)]
        for i in range(0, W, 16):
            cells.append("    " + ", ".join("0x%02X" % b for b in row[i:i + 16]) + ",")
    return "\n".join(cells)


def main():
    source = """/* main_icons.c - 40x14 solid battery icon, LVGL v9 A8 bitmap.
 * Regenerate with tools/gen_main_icons.py (preserves this icon).
 * Direction arrows are text glyphs in main_screen.c, not bitmaps. */
#include "lvgl.h"
#include "main_icons.h"

LV_ATTRIBUTE_MEM_ALIGN static const uint8_t main_icon_battery_data[] = {
%s
};

const lv_image_dsc_t main_icon_battery = {
    .header.cf = LV_COLOR_FORMAT_A8,
    .header.w = 40,
    .header.h = 14,
    .data_size = sizeof(main_icon_battery_data),
    .data = main_icon_battery_data,
};
""" % render_data()
    header = """/* main_icons.h - 40x14 main-screen battery bitmap */
#ifndef MAIN_ICONS_H
#define MAIN_ICONS_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_image_dsc_t main_icon_battery;

#ifdef __cplusplus
}
#endif
#endif /* MAIN_ICONS_H */
"""
    # 换行风格跟随当前文件：仓库里存的是 LF，但 Keil/编辑器可能已改成 CRLF。
    # 不要硬编码某一种，否则单纯跑一次脚本就会把整个文件标成"已修改"。
    target_c = OUT_DIR / "main_icons.c"
    existing = target_c.read_bytes() if target_c.exists() else b""
    nl = "\r\n" if b"\r\n" in existing else "\n"

    with open(target_c, "w", encoding="utf-8", newline=nl) as f:
        f.write(source)
    with open(OUT_DIR / "main_icons.h", "w", encoding="utf-8", newline=nl) as f:
        f.write(header)


if __name__ == "__main__":
    main()
