#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Regenerate the 40x14 main-screen battery outline (A8 for LVGL 9).
The < and > direction arrows use the built-in u14b font, not bitmap assets.
"""
from pathlib import Path

OUT_DIR = Path(__file__).resolve().parent.parent

def battery_pixel(x, y):
    # Left-side terminal; 1px frame. Match the uploaded 40x14 image exactly.
    if y in (0, 13):
        return 255 if 4 <= x <= 38 else 0
    if y in (3, 10):
        return 255 if 1 <= x <= 3 or x == 39 else 0
    if 4 <= y <= 9:
        return 255 if x == 0 or x == 39 else 0
    return 255 if x == 3 or x == 39 else 0

def render_data():
    cells = []
    for y in range(14):
        row = [battery_pixel(x, y) for x in range(40)]
        for i in range(0, 40, 16):
            cells.append("    " + ", ".join("0x%02X" % b for b in row[i:i + 16]) + ",")
    return "\n".join(cells)

def main():
    source = """/* main_icons.c - 40x14 battery outline, LVGL v9 A8 bitmap.
 * Regenerate with tools/gen_main_icons.py (preserves this outline).
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
    (OUT_DIR / "main_icons.c").write_text(source, encoding="utf-8")
    (OUT_DIR / "main_icons.h").write_text(header, encoding="utf-8")

if __name__ == "__main__":
    main()
