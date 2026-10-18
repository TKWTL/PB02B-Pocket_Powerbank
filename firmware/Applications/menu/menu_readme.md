# Menu Framework · 使用与移植

## 文件与职责

- `menu.h/.c`：菜单项/页面数据结构、导航栈、状态机与输入动作。
- `menu_pages.h/.c`：唯一页面树、菜单项实例、应用回调、英文键/中文翻译表和语言选择。
- `menu_ui.c`：LVGL TEXT/ICON/WORD 渲染、布局、滚动动画、选择项编辑。
- `menu_theme.c/.h`：主题色与亮/暗模式。
- `menu_icons.c/.h`：根图标 A8 资源。
- `Applications/fonts/lv_font_menucn_12.c`：Fusion Pixel 12px 1bpp 精简中文+ASCII字形；生成脚本位于 `tools/fonts/build_lvgl_font.py`。
- `ui_task.c`：FreeRTOS界面线程，按键事件→菜单/应用；不要在此执行SW6306等慢速I²C操作。

## 新增菜单

1. 在 `menu_pages.c` 的页面前向声明区域声明 `menu_page_t menu_page_xxx;`。
2. 定义静态 `menu_item_t` 数组。使用 `MENU_ITEM_BACK_`、`MENU_ITEM_PAGE_`、`MENU_ITEM_ACTION_`、`MENU_ITEM_TOGGLE_`、`MENU_ITEM_VALUE_`、`MENU_ITEM_ENUM_` 或 `MENU_ITEM_APP_`。
3. 用 `MENU_PAGE_(title, menu_page_xxx, items)` 定义 TEXT 页，再在父页用 `MENU_ITEM_PAGE_(title, &menu_page_xxx)` 链接。
4. `MENU_ITEM_TOGGLE_` 绑定 `uint8_t *`；`MENU_ITEM_VALUE_` 和 `MENU_ITEM_ENUM_` 绑定 `int32_t *`。编辑时步进/取值范围由宏指定。回调类型 `void(menu_item_t *)`，在值变化时执行。
5. 若设置影响硬件，回调仅改RAM或提交线程安全命令，由对应驱动任务执行。例：自动关闭输出的时间、阈值直接引用 `sw6306_algo` RAM配置，并通过回调清空计时器。
6. 工具应用实现 `menu_app_t` 的 create/activate/run/destroy，注册其 .c 到 Keil 工程；WORD页用 `MENU_WORD_INFO_/CONFIRM_/ACTION_`。

例如：
```c
static const menu_item_t items[] = {
    MENU_ITEM_BACK_("Return"),
    MENU_ITEM_TOGGLE_("Example", &example_enable, "On", "Off", example_apply),
    MENU_ITEM_VALUE_("Limit", &example_limit, 0, 100, 4, "mA", example_apply),
};
MENU_PAGE_("Example", menu_page_example, items);
```

## i18n 与字形

英文 `label/title` 即稳定翻译键；在 `menu_tr_table` 添加英文键与中文对应关系，UI通过 `menu_tr()` 显示中文或英文。没有翻译时原样返回键值。`menu_lang_get()` 查询状态；Language 是直接切换语言的 TOGGLE。WORD和应用界面如需汉化，也必须调用 `menu_tr()`。更新中文文案后从 Fusion Pixel BDF **重新生成字符子集**（见字体目录README），否则新汉字可能缺失。

TEXT页右上角索引显示菜单层级，例如 `2/5`、`>2/4`、`>>2/12`。左下标题有基线补偿，修改字体时应检查LABEL实际绘制区域而不是仅调整坐标。

## 移植要点

提供LVGL 9的显示适配、tick、输入事件，确认分辨率160×40、4-SPI/屏幕方向和内存配额。屏幕大小变化时重新审查 `MENU_SCR_W/H`、文字省略/滚动、图标间距和A8缓冲。把芯片专用任务/资源由 `menu_pages.c` 的动作函数替换为目标平台接口；保存 `menu.c` 的页面状态机和 `menu_ui.c` 的渲染契约。

## 回归测试

检查TEXT滚动无丢项、VALUE/ENUM编辑边界、Language实时切换、返回栈、WORD/SOS/小恐龙翻译以及在充放电和深睡唤醒期间UI不访问正在休眠的I²C器件。
