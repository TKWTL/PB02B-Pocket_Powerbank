# 中文菜单像素字体

- `lv_font_menucn_12.c`：来自 Fusion Pixel Font 12px Proportional zh-Hans BDF 的 LVGL 9 单色（1bpp）子集，覆盖菜单中文翻译与 ASCII。
- `LICENSE-OFL`：字体的 SIL Open Font License 1.1 与版权说明。上游：[TakWolf/fusion-pixel-font](https://github.com/TakWolf/fusion-pixel-font)。
- `tools/fonts/build_lvgl_font.py`：可再生的 Python 3.9+ 标准库脚本；上游 BDF 字体源文件不纳入固件仓库。

菜单文本更新后，下载匹配版本的 12px Proportional zh-Hans BDF 或 .bdf.gz，在仓库根目录执行：

```bash
python tools/fonts/build_lvgl_font.py --bdf /path/to/font.bdf --source firmware/Applications/menu/menu_pages.c --output firmware/Applications/fonts/lv_font_menucn_12.c
```

Keil MDK 工程编译上述 C 字体，LVGL 用 `LV_FONT_CUSTOM_DECLARE` 声明。菜单默认中文，System → Language 可切回英文。主界面功率/电压/时钟使用 8px 宽等宽 Terminus 字体，不随菜单字体变化。
