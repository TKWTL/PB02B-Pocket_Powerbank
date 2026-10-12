# 中文菜单像素字体

- `lv_font_menucn_12.c`：来自 Fusion Pixel Font 12px Proportional zh-Hans BDF 的 LVGL 9 单色（1bpp）子集，覆盖菜单中文翻译与 ASCII。
- `LICENSE-OFL`：字体的 SIL Open Font License 1.1 与版权说明。上游：[TakWolf/fusion-pixel-font](https://github.com/TakWolf/fusion-pixel-font)。
- `tools/fonts/build_lvgl_font.py`：可再生的 Python 3.9+ 标准库脚本；上游 BDF 字体源文件不纳入固件仓库。

菜单文本更新后，下载匹配版本的 12px Proportional zh-Hans BDF 或 .bdf.gz，在仓库根目录执行：

```bash
python tools/fonts/build_lvgl_font.py --bdf /path/to/font.bdf --source firmware/Applications/menu/menu_pages.c --output firmware/Applications/fonts/lv_font_menucn_12.c
```

Keil MDK 工程编译上述 C 字体，LVGL 用 `LV_FONT_CUSTOM_DECLARE` 声明。菜单默认中文，System → Language 可切回英文。主界面功率/电压/时钟使用 8px 宽等宽 Terminus 字体，不随菜单字体变化。

## 本次子集调整与中文基线

- 菜单当前翻译表（含五项后续协议策略的预留翻译）决定字符集；保留 ASCII 32–126、°、？。移除已不需要的“硬”。
- 旧版 292 个仍然使用的 Fusion Pixel 字形保持逐位不变。新增 16 个汉字（使压基打救新束求组结计让跃跳隔面）由于本次未取得完整上游 BDF，采用 Noto Sans CJK SC Regular 的 11×11、单色栅格化字形临时补齐（Copyright 2010–2012 Google, SIL OFL 1.1）。与 Fusion Pixel 字体均按本目录 `LICENSE-OFL` 提供的 OFL 1.1 使用，并保留上述来源说明。
- 字体 `.line_height=12`、`.base_line=0`：原始基线 2 下移 2px，只调整中文字形的显示，不改变 TEXT/WORD 菜单动画的 LABEL 坐标或行距。ICON 的中文标题同样使用这套基线。
- 在可获得原始 BDF 时运行生成器（默认 `--baseline-shift 2`）可替换临时补齐的 16 个 Noto 字形为对应的原生 Fusion Pixel 字形。
