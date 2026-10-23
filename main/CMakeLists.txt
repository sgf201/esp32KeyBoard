# main 组件的源文件。
#
# 这里显式列出而不是用 file(GLOB ...)：GLOB 的结果在 CMake 配置阶段就固定下来，
# 新增 .c 文件后即使删除 build 目录重新配置也不会自动纳入（本工程遇到过，
# 表现为链接期 undefined reference 到新文件里的函数）。
# 显式列表虽然要手工维护，但依赖关系明确，不会出现这种隐性问题。
#
# lvgl_esp32_drivers 提供 lvgl_helpers.h、esp32s3_soc 提供 GT911_RST()，
# 二者都是 main.c 直接包含的公共头文件，故放 REQUIRES；
# 蓝牙/HID 只在实现内部使用，放 PRIV_REQUIRES。
idf_component_register(
    SRCS
        "main.c"
        "ble_hid.c"
        "esp_hid_gap.c"
        "kb_keypad.c"
        "lv_font_kb_16.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl_esp32_drivers esp32s3_soc lvgl esp_lcd
    PRIV_REQUIRES nvs_flash esp_timer esp_hid bt driver esp_driver_gpio)
