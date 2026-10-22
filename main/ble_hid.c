/**
 * @file ble_hid.c
 *
 * BLE HID keyboard + mouse implementation on top of the ESP-IDF esp_hid
 * component with the NimBLE host.
 *
 * ESP32-S3 has BLE only (no Bluetooth Classic), so the device exposes HOGP
 * (HID over GATT). Both keyboard and mouse are packed into one composite report
 * map with distinct report IDs, which lets a single BLE connection serve as
 * keyboard *and* trackpad -- exactly what the macro-keypad UI needs.
 */

#include "ble_hid.h"

#include <string.h>

#include "esp_log.h"
#include "esp_hidd.h"
#include "esp_hid_gap.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if CONFIG_BT_NIMBLE_ENABLED
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "store/config/ble_store_config.h"

/* Provided by the NimBLE store config component but not declared in a public
 * header on all IDF versions. */
void ble_store_config_init(void);
#endif

static const char *TAG = "BLE_HID";

/* Report IDs inside our composite descriptor. */
#define REPORT_ID_KEYBOARD 1
#define REPORT_ID_MOUSE    2

/* ---------------------------------------------------------------------------
 * HID report map: keyboard (report 1) + mouse (report 2) in one descriptor.
 *
 * The keyboard part is a standard 6-key rollover boot-style layout so Windows
 * and Android recognise it without any custom driver. The mouse part reports
 * relative X/Y plus three buttons and a wheel.
 * ------------------------------------------------------------------------- */
static const uint8_t hid_report_map[] = {
    /* ---- Keyboard ---- */
    0x05, 0x01,        /* Usage Page (Generic Desktop) */
    0x09, 0x06,        /* Usage (Keyboard) */
    0xA1, 0x01,        /* Collection (Application) */
    0x85, REPORT_ID_KEYBOARD, /*   Report ID (1) */
    0x05, 0x07,        /*   Usage Page (Key Codes) */
    0x19, 0xE0,        /*   Usage Minimum (224) */
    0x29, 0xE7,        /*   Usage Maximum (231) */
    0x15, 0x00,        /*   Logical Minimum (0) */
    0x25, 0x01,        /*   Logical Maximum (1) */
    0x75, 0x01,        /*   Report Size (1) */
    0x95, 0x08,        /*   Report Count (8) */
    0x81, 0x02,        /*   Input (Data, Variable, Absolute) - modifiers */
    0x95, 0x01,        /*   Report Count (1) */
    0x75, 0x08,        /*   Report Size (8) */
    0x81, 0x01,        /*   Input (Constant) - reserved byte */
    0x95, 0x05,        /*   Report Count (5) */
    0x75, 0x01,        /*   Report Size (1) */
    0x05, 0x08,        /*   Usage Page (LEDs) */
    0x19, 0x01,        /*   Usage Minimum (1) */
    0x29, 0x05,        /*   Usage Maximum (5) */
    0x91, 0x02,        /*   Output (Data, Variable, Absolute) - LEDs */
    0x95, 0x01,        /*   Report Count (1) */
    0x75, 0x03,        /*   Report Size (3) */
    0x91, 0x01,        /*   Output (Constant) - LED padding */
    0x95, 0x06,        /*   Report Count (6) */
    0x75, 0x08,        /*   Report Size (8) */
    0x15, 0x00,        /*   Logical Minimum (0) */
    0x25, 0x65,        /*   Logical Maximum (101) */
    0x05, 0x07,        /*   Usage Page (Key Codes) */
    0x19, 0x00,        /*   Usage Minimum (0) */
    0x29, 0x65,        /*   Usage Maximum (101) */
    0x81, 0x00,        /*   Input (Data, Array) - 6 simultaneous keys */
    0xC0,              /* End Collection */

    /* ---- Mouse ---- */
    0x05, 0x01,        /* Usage Page (Generic Desktop) */
    0x09, 0x02,        /* Usage (Mouse) */
    0xA1, 0x01,        /* Collection (Application) */
    0x85, REPORT_ID_MOUSE, /* Report ID (2) */
    0x09, 0x01,        /*   Usage (Pointer) */
    0xA1, 0x00,        /*   Collection (Physical) */
    0x05, 0x09,        /*     Usage Page (Buttons) */
    0x19, 0x01,        /*     Usage Minimum (1) */
    0x29, 0x03,        /*     Usage Maximum (3) */
    0x15, 0x00,        /*     Logical Minimum (0) */
    0x25, 0x01,        /*     Logical Maximum (1) */
    0x95, 0x03,        /*     Report Count (3) */
    0x75, 0x01,        /*     Report Size (1) */
    0x81, 0x02,        /*     Input (Data, Variable, Absolute) - buttons */
    0x95, 0x01,        /*     Report Count (1) */
    0x75, 0x05,        /*     Report Size (5) */
    0x81, 0x01,        /*     Input (Constant) - padding */
    0x05, 0x01,        /*     Usage Page (Generic Desktop) */
    0x09, 0x30,        /*     Usage (X) */
    0x09, 0x31,        /*     Usage (Y) */
    0x09, 0x38,        /*     Usage (Wheel) */
    0x15, 0x81,        /*     Logical Minimum (-127) */
    0x25, 0x7F,        /*     Logical Maximum (127) */
    0x75, 0x08,        /*     Report Size (8) */
    0x95, 0x03,        /*     Report Count (3) */
    0x81, 0x06,        /*     Input (Data, Variable, Relative) */
    0xC0,              /*   End Collection */
    0xC0,              /* End Collection */
};

static esp_hid_raw_report_map_t report_maps[] = {
    {
        .data = hid_report_map,
        .len = sizeof(hid_report_map),
    },
};

static esp_hid_device_config_t hid_config = {
    .vendor_id = 0x303A,          /* Espressif */
    .product_id = 0x4001,
    .version = 0x0100,
    .device_name = "ESP32 MacroPad",
    .manufacturer_name = "espView",
    .serial_number = "0001",
    .report_maps = report_maps,
    .report_maps_len = 1,
};

static esp_hidd_dev_t *s_hid_dev = NULL;
static volatile bool s_connected = false;

#if CONFIG_BT_NIMBLE_ENABLED
#endif

/* Serialise report submission: BLE host calls originate from the LVGL task,
 * the mouse task and possibly timer callbacks, and interleaving gatts notify
 * calls would garble reports. */
static SemaphoreHandle_t s_tx_lock = NULL;

static esp_err_t tx_lock(void)
{
    if (s_tx_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return (xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(100)) == pdTRUE)
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

static void tx_unlock(void)
{
    if (s_tx_lock != NULL) {
        xSemaphoreGive(s_tx_lock);
    }
}

/* ---------------------------------------------------------------------------
 * NimBLE HID gap glue.
 *
 * esp_hid_gap.c (taken from the IDF example) calls these two hooks from its GAP
 * event handler once encryption is established / the link drops. In the
 * Espressif example they spawn a demo task that periodically emits HID reports.
 *
 * Here the reports are driven by the UI (keypad taps, trackpad movements), so
 * there is nothing periodic to start. The hooks still must exist to satisfy the
 * link, and they are a convenient place to log connection state.
 * ------------------------------------------------------------------------- */
void ble_hid_task_start_up(void)
{
    ESP_LOGI(TAG, "HID link ready (encrypted)");
}

void ble_hid_task_shut_down(void)
{
    ESP_LOGI(TAG, "HID link down");
}

/* ---------------------------------------------------------------------------
 * esp_hid event plumbing
 * ------------------------------------------------------------------------- */
static void hid_event_handler(void *handler_args, esp_event_base_t base,
                              int32_t id, void *event_data)
{
    esp_hidd_event_t event = (esp_hidd_event_t)id;
    esp_hidd_event_data_t *param = (esp_hidd_event_data_t *)event_data;

    switch (event) {
    case ESP_HIDD_START_EVENT: {
        ESP_LOGI(TAG, "HID stack started, advertising");
        esp_hid_ble_gap_adv_start();
        break;
    }
    case ESP_HIDD_CONNECT_EVENT:
        s_connected = true;
        /* Encryption is initiated from esp_hid_gap.c's GAP connect handler,
         * which is the only place the BLE connection handle is available. */
        ESP_LOGI(TAG, "host connected");
        break;
    case ESP_HIDD_PROTOCOL_MODE_EVENT:
        ESP_LOGI(TAG, "protocol mode set to %s",
                 esp_hid_protocol_mode_str(param->protocol_mode.protocol_mode));
        break;
    case ESP_HIDD_CONTROL_EVENT:
        ESP_LOGI(TAG, "control event: %s",
                 param->control.control == ESP_HID_CONTROL_SUSPEND
                     ? "suspend" : "exit suspend");
        break;
    case ESP_HIDD_OUTPUT_EVENT:
        /* Host writes LED state here (caps lock etc). Not used yet. */
        break;
    case ESP_HIDD_DISCONNECT_EVENT:
        s_connected = false;
        ESP_LOGI(TAG, "host disconnected, re-advertising");
        esp_hid_ble_gap_adv_start();
        break;
    default:
        ESP_LOGD(TAG, "unhandled HID event %d", (int)event);
        break;
    }
}

#if CONFIG_BT_NIMBLE_ENABLED
/* NimBLE's host stack runs in its own FreeRTOS task, which must be started
 * explicitly. esp_hid_gap.c's init_low_level() only calls esp_nimble_init(),
 * which brings up the controller but leaves the host idle -- without this the
 * device never advertises and no host can ever connect. */
static void nimble_host_task(void *param)
{
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();          /* returns only after nimble_port_stop() */
    nimble_port_freertos_deinit();
}

/* ble_gap_adv_start() fails if called before the host finishes syncing with the
 * controller, so advertising is kicked off from the sync callback instead of
 * racing the host task. */
static void on_nimble_sync(void)
{
    ESP_LOGI(TAG, "NimBLE host synced, starting advertising");
    esp_err_t ret = esp_hid_ble_gap_adv_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "advertising failed: %s", esp_err_to_name(ret));
    }
}

static void on_nimble_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE host reset, reason=%d", reason);
}

/* ---------------------------------------------------------------------------
 * Pairing configuration.
 *
 * esp_hid_gap.c sets IO capability to DISP_ONLY while also requesting MITM
 * protection. Those two contradict each other: MITM requires either a passkey
 * comparison or a PIN, but DISP_ONLY means this device can only display a code
 * and never confirms it. Windows aborts the pairing with
 * "please try connecting to the device again".
 *
 * A HID keyboard that wants to attach to desktops and phones should use
 * NO_IO + Just Works: no MITM, bonding enabled, encryption still required.
 * That is what commercial BLE keyboards do, and it pairs without any prompt.
 * ------------------------------------------------------------------------- */
static void configure_pairing(void)
{
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;      /* remember the host so re-pairing is not needed */
    ble_hs_cfg.sm_mitm = 0;         /* no MITM: nothing to compare a passkey against */
    ble_hs_cfg.sm_sc = 1;           /* LE Secure Connections when the host supports it */
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ID | BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ID | BLE_SM_PAIR_KEY_DIST_ENC;

    /* Without an explicit store the host keys are dropped and every reconnect
     * turns into a fresh pairing attempt (which Windows then rejects). */
    ble_store_config_init();
}
#endif

/* ---------------------------------------------------------------------------
 * Advertising keep-alive.
 *
 * The NimBLE advertising started by esp_hid_ble_gap_adv_start() is time-boxed
 * (180s in the IDF example) and esp_hid_gap.c does not restart it after
 * BLE_GAP_EVENT_ADV_COMPLETE or a disconnect. Without a supervisor the device
 * silently stops being discoverable, so this task re-arms advertising whenever
 * nobody is connected.
 * ------------------------------------------------------------------------- */
#define ADV_KEEPALIVE_PERIOD_MS 5000

static void adv_keepalive_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(ADV_KEEPALIVE_PERIOD_MS));
        if (s_connected) {
            continue;
        }
#if CONFIG_BT_NIMBLE_ENABLED
        /* Only re-arm when the stack is not already advertising, otherwise
         * every tick would log an "already advertising" error. */
        if (ble_gap_adv_active()) {
            continue;
        }
        ESP_LOGI(TAG, "advertising stopped, re-arming");
#endif
        esp_hid_ble_gap_adv_start();
    }
}

/* ---------------------------------------------------------------------------
 * Security initiation.
 *
 * esp_hid_gap.c logs the connection and immediately returns without asking for
 * encryption. HID input reports live behind a "encrypted link required"
 * characteristic, so a host that connects unencrypted (Windows does) cannot
 * subscribe; Windows then tears the link down ~90ms later with
 * reason=531 (HCI 0x13, "remote user terminated connection") and retries
 * forever. The peripheral is expected to drive pairing, so kick it off here.
 * ------------------------------------------------------------------------- */

esp_err_t ble_hid_init(void)
{
    esp_err_t ret;

    if (s_hid_dev != NULL) {
        return ESP_OK;
    }

    s_tx_lock = xSemaphoreCreateMutex();
    if (s_tx_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ret = esp_hid_gap_init(HIDD_BLE_MODE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_hid_gap_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_hid_ble_gap_adv_init(ESP_HID_APPEARANCE_KEYBOARD,
                                   hid_config.device_name);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "adv init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_hidd_dev_init(&hid_config, ESP_HID_TRANSPORT_BLE,
                            hid_event_handler, &s_hid_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_hidd_dev_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

#if CONFIG_BT_NIMBLE_ENABLED
    /* Register sync/reset hooks *before* the host task runs so the sync
     * callback is guaranteed to fire. */
    ble_hs_cfg.sync_cb = on_nimble_sync;
    ble_hs_cfg.reset_cb = on_nimble_reset;
    configure_pairing();
    nimble_port_freertos_init(nimble_host_task);
#endif

    ESP_LOGI(TAG, "BLE HID keyboard+mouse ready, name=\"%s\"",
             hid_config.device_name);

    xTaskCreatePinnedToCore(adv_keepalive_task, "adv_keepalive", 1024 * 3,
                            NULL, 2, NULL, 0);
    return ESP_OK;
}

bool ble_hid_is_connected(void)
{
    return s_connected && s_hid_dev != NULL;
}

/* ---------------------------------------------------------------------------
 * Report senders
 * ------------------------------------------------------------------------- */

/* Keyboard input report: [modifiers, reserved, key1..key6] */
static esp_err_t send_keyboard(uint8_t modifiers, const uint8_t keys[6])
{
    if (s_hid_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t report[8] = {0};
    report[0] = modifiers;
    report[1] = 0;
    for (int i = 0; i < 6; i++) {
        report[2 + i] = keys[i];
    }

    esp_err_t ret = tx_lock();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = esp_hidd_dev_input_set(s_hid_dev, 0, REPORT_ID_KEYBOARD,
                                 report, sizeof(report));
    tx_unlock();

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "keyboard report failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t ble_hid_key_combo(uint8_t modifiers, uint8_t keycode)
{
    uint8_t keys[6] = {0};
    keys[0] = keycode;
    return send_keyboard(modifiers, keys);
}

esp_err_t ble_hid_key_down(uint8_t modifiers, const uint8_t keycodes[6])
{
    uint8_t keys[6] = {0};
    if (keycodes != NULL) {
        memcpy(keys, keycodes, sizeof(keys));
    }
    return send_keyboard(modifiers, keys);
}

esp_err_t ble_hid_key_release_all(void)
{
    uint8_t keys[6] = {0};
    return send_keyboard(0, keys);
}

esp_err_t ble_hid_key_tap(uint8_t keycode, uint8_t modifiers)
{
    esp_err_t ret = ble_hid_key_combo(modifiers, keycode);
    if (ret != ESP_OK) {
        return ret;
    }
    /* Give the host a moment to register the press before the release,
     * otherwise fast hosts coalesce both into "no key". */
    vTaskDelay(pdMS_TO_TICKS(12));
    return ble_hid_key_release_all();
}

/* Mouse input report: [buttons, dx, dy, wheel] */
static esp_err_t send_mouse(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel)
{
    if (s_hid_dev == NULL || !s_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t report[4] = {buttons, (uint8_t)dx, (uint8_t)dy, (uint8_t)wheel};

    esp_err_t ret = tx_lock();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = esp_hidd_dev_input_set(s_hid_dev, 0, REPORT_ID_MOUSE,
                                 report, sizeof(report));
    tx_unlock();
    return ret;
}

esp_err_t ble_hid_mouse_move(int8_t dx, int8_t dy, uint8_t buttons)
{
    return send_mouse(buttons, dx, dy, 0);
}

esp_err_t ble_hid_mouse_click(uint8_t button)
{
    esp_err_t ret = send_mouse(button, 0, 0, 0);
    if (ret != ESP_OK) {
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(15));
    return send_mouse(0, 0, 0, 0);
}

esp_err_t ble_hid_mouse_wheel(int8_t steps)
{
    esp_err_t ret = send_mouse(0, 0, 0, steps);
    if (ret != ESP_OK) {
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    return send_mouse(0, 0, 0, 0);
}

esp_err_t ble_hid_set_battery(uint8_t level)
{
    if (s_hid_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (level > 100) {
        level = 100;
    }
    return esp_hidd_dev_battery_set(s_hid_dev, level);
}
