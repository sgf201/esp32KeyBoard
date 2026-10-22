/**
 * @file ble_hid.h
 *
 * BLE HID keyboard + mouse for the macro-keypad project.
 *
 * ESP32-S3 only has BLE (no Bluetooth Classic), so this uses HOGP over GATT via
 * the NimBLE stack. Both the keyboard and the mouse live in a single composite
 * HID descriptor, which Windows and Android both accept as one device.
 */
#ifndef BLE_HID_H
#define BLE_HID_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* HID modifier bits, matching the standard keyboard boot report. */
#define BLE_HID_MOD_LCTRL   0x01
#define BLE_HID_MOD_LSHIFT  0x02
#define BLE_HID_MOD_LALT    0x04
#define BLE_HID_MOD_LGUI    0x08
#define BLE_HID_MOD_RCTRL  0x10
#define BLE_HID_MOD_RSHIFT  0x20
#define BLE_HID_MOD_RALT    0x40
#define BLE_HID_MOD_RGUI    0x80

/* HID usage IDs for the keys we actually need. */
#define BLE_HID_KEY_A        0x04
#define BLE_HID_KEY_C        0x06
#define BLE_HID_KEY_V        0x19
#define BLE_HID_KEY_X        0x1B
#define BLE_HID_KEY_Z        0x1D
#define BLE_HID_KEY_S        0x16
#define BLE_HID_KEY_ENTER    0x28
#define BLE_HID_KEY_ESC      0x29
#define BLE_HID_KEY_TAB      0x2B
#define BLE_HID_KEY_SPACE    0x2C
#define BLE_HID_KEY_DELETE   0x4C

/* Mouse buttons */
#define BLE_HID_MOUSE_LEFT   0x01
#define BLE_HID_MOUSE_RIGHT  0x02
#define BLE_HID_MOUSE_MIDDLE 0x04

/**
  * @brief  Start the BLE HID stack and begin advertising as a keyboard+mouse.
  *
  * Must be called after nvs_flash_init(). Returns immediately; the stack comes
  * up asynchronously and ble_hid_is_connected() tells you when a host attached.
  */
esp_err_t ble_hid_init(void);

/**
  * @brief  True once a host (PC / phone) has connected and subscribed.
  */
bool ble_hid_is_connected(void);

/**
  * @brief  Type a single key: press then release, automatically.
  *
  * @param  keycode: HID usage ID (see BLE_HID_KEY_*).
  * @param  modifiers: bitmask of BLE_HID_MOD_*; pass 0 for plain typing.
  */
esp_err_t ble_hid_key_tap(uint8_t keycode, uint8_t modifiers);

/**
  * @brief  Send a full key combination in one report, e.g. Ctrl+Shift+Esc.
  *
  * @param  modifiers: bitmask of BLE_HID_MOD_*.
  * @param  keycode:   HID usage ID, or 0 for a modifier-only combo.
  */
esp_err_t ble_hid_key_combo(uint8_t modifiers, uint8_t keycode);

/**
  * @brief  Hold keys down without releasing (for modifier latches).
  *
  * @param  keycodes: up to 6 usage IDs; unused slots must be 0.
  */
esp_err_t ble_hid_key_down(uint8_t modifiers, const uint8_t keycodes[6]);

/**
  * @brief  Release all currently held keys.
  */
esp_err_t ble_hid_key_release_all(void);

/**
  * @brief  Send a relative mouse movement plus button state.
  *
  * @param  dx, dy: relative movement in pixels (-127..127 per report).
  * @param  buttons: bitmask of BLE_HID_MOUSE_*.
  */
esp_err_t ble_hid_mouse_move(int8_t dx, int8_t dy, uint8_t buttons);

/**
  * @brief  Click a mouse button (press + release).
  */
esp_err_t ble_hid_mouse_click(uint8_t button);

/**
  * @brief  Send a vertical wheel step.
  *
  * @param  steps: positive scrolls up, negative scrolls down.
  */
esp_err_t ble_hid_mouse_wheel(int8_t steps);

/**
  * @brief  Publish the battery level to the host (0-100).
  *
  * HID exposes a Battery Service; if it is never written the host reads 0 and
  * reports "battery low". This board is USB powered, so report 100.
  */
esp_err_t ble_hid_set_battery(uint8_t level);

#ifdef __cplusplus
}
#endif

#endif /* BLE_HID_H */
