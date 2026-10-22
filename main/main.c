#include <stdio.h>
#include "lvgl_helpers.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "lvgl/demos/lv_demos.h"
#include "freertos/semphr.h"
#include "esp32s3_soc.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "lvgl.h"

#include "ble_hid.h"
#include "kb_keypad.h"
#include "nvs_flash.h"

static const char *TAG = "https://hemukeji.taobao.com/";

#define LVGL_TICK_PERIOD_MS 2

void lvgl_task(void *pvParameter);
SemaphoreHandle_t xGuiSemaphore;
static void increase_lvgl_tick(void *arg)
{
    /* Tell LVGL how many milliseconds has elapsed */
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

void lvgl_task(void *pvParameter)
{
    gpio_set_level(6, 1);                    // 开启背光
    xGuiSemaphore = xSemaphoreCreateMutex(); // 创建GUI信号量
    lv_init();                               // lvgl初始化
    lvgl_driver_init();                      // 初始化液晶驱动

    static lv_disp_draw_buf_t disp_buf;
    /* Draw buffer: the original code allocated two DMA-capable buffers and used
     * them without checking for failure. Once the BLE stack claims its memory,
     * these allocations can return NULL and LVGL then writes through a null
     * pointer (StoreProhibited inside lv_color_fill). Request the buffers with
     * a PSRAM fallback and fail loudly instead of corrupting memory. */
    size_t buf_bytes = DISP_BUF_SIZE * 2;
    lv_color_t *buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM);
    lv_color_t *buf2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM);
    if (buf1 == NULL || buf2 == NULL) {
        ESP_LOGW(TAG, "PSRAM draw buffers unavailable (%p/%p), retrying internal",
                 buf1, buf2);
        if (buf1 == NULL) {
            buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA);
        }
        if (buf2 == NULL) {
            buf2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA);
        }
    }
    if (buf1 == NULL || buf2 == NULL) {
        ESP_LOGE(TAG, "failed to allocate LVGL draw buffers (%u bytes each), "
                      "free heap: %u", (unsigned)buf_bytes,
                 (unsigned)esp_get_free_heap_size());
        vTaskDelay(pdMS_TO_TICKS(1000));
        abort();
    }
    ESP_LOGI(TAG, "draw buffers: %u bytes each, free heap %u",
             (unsigned)buf_bytes, (unsigned)esp_get_free_heap_size());
    // initialize LVGL draw buffers
    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, LCD_V_RES * 50);
    // 添加并注册触摸驱动
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_H_RES;
    disp_drv.ver_res = LCD_V_RES;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.user_data = panel_handle;
    lv_disp_drv_register(&disp_drv);

#if CONFIG_LV_TOUCH_CONTROLLER != TOUCH_CONTROLLER_NONE
    lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.read_cb = touch_driver_read;
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    lv_indev_drv_register(&indev_drv);
#endif

    /* 创建一个10ms定时器*/ // 定期处理GUI回调
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &increase_lvgl_tick,
        .name = "lvgl_tick"};
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, LVGL_TICK_PERIOD_MS * 1000));

    /* 宏键盘界面（取代原来的 lv_demo_widgets 演示） */
    kb_ui_create();

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(10)); /* Delay 1 tick (assumes FreeRTOS tick is 10ms */
        if (pdTRUE == xSemaphoreTake(xGuiSemaphore, portMAX_DELAY))
        {
            lv_timer_handler();            // 处理LVGL任务
            xSemaphoreGive(xGuiSemaphore); // 释放信号量
        }
    }
}

/* ---------------------------------------------------------------------------
 * Multi-touch observer.
 *
 * gt911_read() (the LVGL input driver) is the only consumer allowed to poll the
 * controller, because the GT911 status register is read-to-clear. This task
 * therefore just snapshots the last sample via gt911_get_last_sample() and logs
 * how many fingers are down -- useful for verifying the 5-point path without
 * disturbing normal UI input.
 * ------------------------------------------------------------------------- */
#define MT_LOG_INTERVAL_MS 100
#define MT_MAX_REPORT      5

static void multitouch_task(void *arg)
{
    gt911_touch_data_t td;
    uint8_t last_count = 0;
    TickType_t last_log = 0;

    while (!gt911_status.inited) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGI("MT", "multi-touch observer started");

    while (1) {
        if (gt911_get_last_sample(&td)) {
            bool changed = (td.count != last_count);
            if (changed || (xTaskGetTickCount() - last_log) >= pdMS_TO_TICKS(MT_LOG_INTERVAL_MS)) {
                if (td.count > 0) {
                    char line[192];
                    int n = snprintf(line, sizeof(line), "MT points=%u", td.count);
                    for (uint8_t i = 0; i < td.count && i < MT_MAX_REPORT; i++) {
                        n += snprintf(line + n, sizeof(line) - n, " [%u:id%u %ux%u sz%u]",
                                      i, td.points[i].track_id,
                                      td.points[i].x, td.points[i].y,
                                      td.points[i].size);
                    }
                    ESP_LOGI("MT", "%s", line);
                } else if (changed) {
                    ESP_LOGI("MT", "MT points=0 (released)");
                }
                last_count = td.count;
                last_log = xTaskGetTickCount();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

/* ---------------------------------------------------------------------------
 * HID link supervisor.
 *
 * The earlier self-test task typed "a" / "hi" and moved the mouse in a loop,
 * which is obviously disruptive once a real host is attached. It is replaced
 * by this supervisor, whose only jobs are:
 *   - publish a battery level (without it the HID BAS characteristic stays at
 *     0 and Windows reports "battery low"),
 *   - refresh that level periodically so the host keeps the entry alive.
 * ------------------------------------------------------------------------- */
#define HID_BATTERY_LEVEL   100     /* board is USB powered; report full */
#define HID_BATTERY_PERIOD_MS 60000

static void hid_supervisor_task(void *arg)
{
    bool was_connected = false;

    while (1) {
        bool now_connected = ble_hid_is_connected();

        if (now_connected && !was_connected) {
            ESP_LOGI("HID", "host connected, reporting battery %d%%",
                     HID_BATTERY_LEVEL);
            ble_hid_set_battery(HID_BATTERY_LEVEL);
        } else if (now_connected) {
            /* Keep the level fresh; some hosts drop the entry if it goes stale. */
            ble_hid_set_battery(HID_BATTERY_LEVEL);
        } else if (!now_connected && was_connected) {
            ESP_LOGW("HID", "host disconnected");
        }

        was_connected = now_connected;
        vTaskDelay(pdMS_TO_TICKS(now_connected ? HID_BATTERY_PERIOD_MS : 500));
    }
}

void app_main(void)
{
    /* BLE needs NVS for bonding information. */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    GT911_RST();
    xTaskCreatePinnedToCore(lvgl_task, "lvgl_task", 1024 * 6, NULL, 5, NULL, 1);

    /* Verify the GT911 5-point path runs parallel to the normal LVGL input. */
    xTaskCreatePinnedToCore(multitouch_task, "multitouch", 1024 * 4, NULL, 4, NULL, 0);

    /* BLE HID keyboard + mouse. */
    ret = ble_hid_init();
    if (ret != ESP_OK) {
        ESP_LOGE("HID", "ble_hid_init failed: %s", esp_err_to_name(ret));
    } else {
        xTaskCreatePinnedToCore(hid_supervisor_task, "hid_sup", 1024 * 4, NULL, 3, NULL, 0);
    }
}
