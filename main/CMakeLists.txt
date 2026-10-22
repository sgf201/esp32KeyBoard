file(GLOB_RECURSE SOURCES_C *.c)
file(GLOB_RECURSE SOURCES_CPP *.cpp)

# lvgl_esp32_drivers 提供 lvgl_helpers.h、esp32s3_soc 提供 GT911_RST()，
# 二者都是 main.c 直接包含的公共头文件，故放 REQUIRES；
# 蓝牙/HID 只在实现内部使用，放 PRIV_REQUIRES。
idf_component_register(
    SRCS ${SOURCES_C} ${SOURCES_CPP}
    INCLUDE_DIRS "."
    REQUIRES lvgl_esp32_drivers esp32s3_soc lvgl esp_lcd
    PRIV_REQUIRES nvs_flash esp_timer esp_hid bt driver)
