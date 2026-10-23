#ifndef __GT911_H
/*
* Copyright © 2021 Sturnus Inc.

* Permission is hereby granted, free of charge, to any person obtaining a copy of this 
* software and associated documentation files (the “Software”), to deal in the Software 
* without restriction, including without limitation the rights to use, copy, modify, merge, 
* publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons 
* to whom the Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in all copies or 
* substantial portions of the Software.
* 
* THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, 
* INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR 
* PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE 
* FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, 
* ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE 
* SOFTWARE.
*/

#define __GT911_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define GT911_I2C_SLAVE_ADDR   0x5D

#define GT911_PRODUCT_ID_LEN   4

/* Register Map of GT911 */
#define GT911_PRODUCT_ID1             0x8140
#define GT911_PRODUCT_ID2             0x8141
#define GT911_PRODUCT_ID3             0x8142
#define GT911_PRODUCT_ID4             0x8143
#define GT911_FIRMWARE_VER_L          0x8144
#define GT911_FIRMWARE_VER_H          0x8145
#define GT911_X_COORD_RES_L           0x8146
#define GT911_X_COORD_RES_H           0x8147
#define GT911_Y_COORD_RES_L           0x8148
#define GT911_Y_COORD_RES_H           0x8149
#define GT911_VENDOR_ID               0x814A

#define GT911_STATUS_REG              0x814E
#define   GT911_STATUS_REG_BUF        0x80
#define   GT911_STATUS_REG_LARGE      0x40
#define   GT911_STATUS_REG_PROX_VALID 0x20
#define   GT911_STATUS_REG_HAVEKEY    0x10
#define   GT911_STATUS_REG_PT_MASK    0x0F

#define GT911_TRACK_ID1               0x814F
#define GT911_PT1_X_COORD_L           0x8150
#define GT911_PT1_X_COORD_H           0x8151
#define GT911_PT1_Y_COORD_L           0x8152
#define GT911_PT1_Y_COORD_H           0x8153
#define GT911_PT1_X_SIZE_L            0x8154
#define GT911_PT1_X_SIZE_H            0x8155

/* GT911 reports up to 5 touch points, laid out contiguously starting at
 * GT911_TRACK_ID1 (0x814F). Each point occupies 8 bytes:
 *
 *   +0 : track id
 *   +1 : x low      +2 : x high
 *   +3 : y low      +4 : y high
 *   +5 : size low   +6 : size high
 *   +7 : reserved
 *
 * So all 5 points can be fetched with a single 40-byte I2C read instead of
 * 40 individual 1-byte reads. */
#define GT911_MAX_TOUCH_POINTS   5
#define GT911_POINT_SIZE         8
#define GT911_POINTS_BUF_SIZE    (GT911_MAX_TOUCH_POINTS * GT911_POINT_SIZE)

typedef struct {
    bool inited;
    char product_id[GT911_PRODUCT_ID_LEN];
    uint16_t max_x_coord;
    uint16_t max_y_coord;
    uint8_t i2c_dev_addr;
} gt911_status_t;

/* Driver state, defined in gt911.c. Exposed so callers can wait for the
 * controller to finish initialising before polling it. */
extern gt911_status_t gt911_status;

/* A single decoded touch point, already run through the same axis
 * transformations (SWAPXY / INVERT_X / INVERT_Y) the single-touch path uses,
 * so coordinates are directly usable as LVGL screen coordinates. */
typedef struct {
    uint8_t track_id;   /* hardware track id, stable while the finger stays down */
    uint16_t x;
    uint16_t y;
    uint16_t size;      /* contact area reported by the controller */
} gt911_touch_point_t;

typedef struct {
    uint8_t count;                                  /* number of valid points (0..5) */
    gt911_touch_point_t points[GT911_MAX_TOUCH_POINTS];
} gt911_touch_data_t;

/**
  * @brief  Read all active touch points in one I2C transaction.
  *
  * Unlike gt911_read() (which intentionally drops everything but a single
  * finger), this returns every point the controller is reporting, which is
  * what multi-finger UIs need.
  *
  * @param  out: filled with the point count and coordinates. When no finger is
  *              down, count is 0.
  * @retval ESP_OK on success, otherwise the I2C error.
  */
esp_err_t gt911_read_points(gt911_touch_data_t *out);

/**
  * @brief  Copy the most recent touch sample without touching the I2C bus.
  *
  * The GT911 status register is read-to-clear, so only one consumer may poll
  * the controller. Any observer that must not disturb sampling should call this,
  * which just snapshots whatever gt911_read_points()/gt911_sample_now() produced.
  *
  * @param  out: receives a copy of the last sample.
  * @retval true if a sample has been recorded at least once.
  */
bool gt911_get_last_sample(gt911_touch_data_t *out);

/**
  * @brief  Sample the controller now and return the result (recommended API).
  *
  * Serialises the whole I2C transaction with an internal mutex, so a single task
  * can safely be the only reader. When the LVGL indev is not registered (this
  * project skips it to gain real multi-touch), nothing else ever calls
  * gt911_read_points(), so this function MUST be used or the snapshot stays
  * empty and touch appears dead.
  *
  * @param  out: receives this sample.
  * @retval ESP_OK on success, otherwise a timeout or I2C error.
  */
esp_err_t gt911_sample_now(gt911_touch_data_t *out);

/**
  * @brief  Create the I2C access mutex (optional; gt911_sample_now does it).
  */
void gt911_i2c_lock_init(void);

/**
  * @brief  Read the raw status register (0x814E) for diagnostics.
  *
  * @retval status byte; 0xFF if not initialised, 0xFE if the I2C read failed.
  */
uint8_t gt911_debug_status(void);

/**
  * @brief  Dump a handful of key registers to the log (diagnostics).
  */
void gt911_debug_dump(void);

/**
  * @brief  Dump the raw 44-byte touch buffer starting at the status register.
  *
  * Consumes one touch event (the status read acknowledges it), so call this only
  * while diagnosing a "chip answers I2C but never reports points" problem.
  */
void gt911_debug_raw_points(void);

/**
  * @brief  Software-reset the controller and wait for it to settle.
  *
  * Useful when the chip answers I2C but reports no touch events at all
  * (status register stuck at 0x00).
  */
esp_err_t gt911_soft_reset(void);

/**
  * @brief  Initialize for GT911 communication via I2C
  * @param  dev_addr: Device address on communication Bus (I2C slave address of GT911).
  * @retval None
  */
void gt911_init(uint8_t dev_addr);

/**
  * @brief  Get the touch screen X and Y positions values. Ignores multi touch
  * @param  drv:
  * @param  data: Store data here
  * @retval Always false
  */
bool gt911_read(lv_indev_drv_t *drv, lv_indev_data_t *data);

#ifdef __cplusplus
}
#endif
#endif /* __GT911_H */
