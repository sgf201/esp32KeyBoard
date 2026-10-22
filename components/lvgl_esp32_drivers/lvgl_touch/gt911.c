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

#include <esp_log.h>
#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include <lvgl.h>
#else
#include <lvgl/lvgl.h>
#endif
#include "gt911.h"

#include "lvgl_i2c/i2c_manager.h"

#define TAG "GT911"
#if CONFIG_LV_I2C_TOUCH_PORT_0
    #undef CONFIG_LV_I2C_TOUCH_PORT
    #define CONFIG_LV_I2C_TOUCH_PORT    0
#elif     CONFIG_LV_I2C_TOUCH_PORT_1
    #undef CONFIG_LV_I2C_TOUCH_PORT
    #define CONFIG_LV_I2C_TOUCH_PORT    1
#endif

gt911_status_t gt911_status;

/* Last sample produced by gt911_read_points(). Guarded by a spinlock because
 * the LVGL input callback (task context) writes it while observer tasks read
 * it. Deliberately separate from the I2C access so observers never disturb the
 * read-to-clear status register. */
static gt911_touch_data_t s_last_sample;
static portMUX_TYPE s_sample_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_sample_valid = false;

bool gt911_get_last_sample(gt911_touch_data_t *out)
{
    if (out == NULL) {
        return false;
    }
    bool valid;
    taskENTER_CRITICAL(&s_sample_mux);
    *out = s_last_sample;
    valid = s_sample_valid;
    taskEXIT_CRITICAL(&s_sample_mux);
    return valid;
}

//TODO: handle multibyte read and refactor to just one read transaction
esp_err_t gt911_i2c_read(uint8_t slave_addr, uint16_t register_addr, uint8_t *data_buf, uint8_t len) {
    //return lvgl_i2c_read(CONFIG_LV_I2C_TOUCH_PORT, slave_addr, register_addr | I2C_REG_16, data_buf, len);
    return I2C_FN(_read)(0, slave_addr, register_addr | I2C_REG_16, data_buf, len);
}

esp_err_t gt911_i2c_write8(uint8_t slave_addr, uint16_t register_addr, uint8_t data) {
    uint8_t buffer = data;
    //return lvgl_i2c_write(CONFIG_LV_I2C_TOUCH_PORT, slave_addr, register_addr | I2C_REG_16, &buffer, 1);
    return I2C_FN(_write)(0, slave_addr, register_addr | I2C_REG_16, &buffer, 1);
}

/**
  * @brief  Initialize for GT911 communication via I2C
  * @param  dev_addr: Device address on communication Bus (I2C slave address of GT911).
  * @retval None
  */
void gt911_init(uint8_t dev_addr) {
    if (!gt911_status.inited) {
        gt911_status.i2c_dev_addr = dev_addr;
        uint8_t data_buf;
        esp_err_t ret;

        // ESP_LOGI(TAG, "Checking for GT911 Touch Controller");
        if ((ret = gt911_i2c_read(dev_addr, GT911_PRODUCT_ID1, &data_buf, 1) != ESP_OK)) {
            ESP_LOGE(TAG, "Error reading from device: %s",
                        esp_err_to_name(ret));    // Only show error the first time
            return;
        }

        // Read 4 bytes for Product ID in ASCII
        for (int i = 0; i < GT911_PRODUCT_ID_LEN; i++) {
            gt911_i2c_read(dev_addr, (GT911_PRODUCT_ID1 + i), (uint8_t *)&(gt911_status.product_id[i]), 1);
        }
        ESP_LOGI(TAG, "\tProduct ID: %s", gt911_status.product_id);

        gt911_i2c_read(dev_addr, GT911_VENDOR_ID, &data_buf, 1);
        ESP_LOGI(TAG, "\tVendor ID: 0x%02x", data_buf);

        gt911_i2c_read(dev_addr, GT911_X_COORD_RES_L, &data_buf, 1);
        gt911_status.max_x_coord = data_buf;
        gt911_i2c_read(dev_addr, GT911_X_COORD_RES_H, &data_buf, 1);
        gt911_status.max_x_coord |= ((uint16_t)data_buf << 8);
        ESP_LOGI(TAG, "\tX Resolution: %d", gt911_status.max_x_coord);

        gt911_i2c_read(dev_addr, GT911_Y_COORD_RES_L, &data_buf, 1);
        gt911_status.max_y_coord = data_buf;
        gt911_i2c_read(dev_addr, GT911_Y_COORD_RES_H, &data_buf, 1);
        gt911_status.max_y_coord |= ((uint16_t)data_buf << 8);
        ESP_LOGI(TAG, "\tY Resolution: %d", gt911_status.max_y_coord);
        gt911_status.inited = true;
    }
}

/**
  * @brief  Read every active touch point in a single I2C transaction.
  *
  * The controller lays the 5 point records out contiguously from 0x814F, so one
  * 40-byte read replaces what used to be dozens of 1-byte reads. The status
  * register must be cleared afterwards, otherwise GT911 keeps reporting the
  * same buffer-ready flag and stops updating coordinates.
  */
esp_err_t gt911_read_points(gt911_touch_data_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    out->count = 0;

    if (!gt911_status.inited) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t status_reg = 0;
    esp_err_t ret = gt911_i2c_read(gt911_status.i2c_dev_addr, GT911_STATUS_REG,
                                   &status_reg, 1);
    if (ret != ESP_OK) {
        return ret;
    }

    /* Bit 7 = buffer ready; low nibble = number of points being reported. */
    uint8_t count = status_reg & GT911_STATUS_REG_PT_MASK;
    if ((status_reg & GT911_STATUS_REG_BUF) == 0 || count == 0) {
        /* Nothing new. Still clear the flag if the controller asserted it. */
        if (status_reg & GT911_STATUS_REG_BUF) {
            gt911_i2c_write8(gt911_status.i2c_dev_addr, GT911_STATUS_REG, 0x00);
        }
        /* Publish the "no fingers" state for observers, but keep the previous
         * coordinates so a momentary gap does not look like a release. */
        taskENTER_CRITICAL(&s_sample_mux);
        s_last_sample.count = 0;
        s_sample_valid = true;
        taskEXIT_CRITICAL(&s_sample_mux);
        return ESP_OK;
    }

    if (count > GT911_MAX_TOUCH_POINTS) {
        count = GT911_MAX_TOUCH_POINTS;
    }

    /* Fetch all points at once: track id, x, y, size for each. */
    uint8_t buf[GT911_POINTS_BUF_SIZE];
    ret = gt911_i2c_read(gt911_status.i2c_dev_addr, GT911_TRACK_ID1, buf,
                         GT911_POINTS_BUF_SIZE);
    if (ret != ESP_OK) {
        gt911_i2c_write8(gt911_status.i2c_dev_addr, GT911_STATUS_REG, 0x00);
        return ret;
    }

    for (uint8_t i = 0; i < count; i++) {
        const uint8_t *p = &buf[i * GT911_POINT_SIZE];

        int16_t x = (int16_t)((uint16_t)p[1] | ((uint16_t)p[2] << 8));
        int16_t y = (int16_t)((uint16_t)p[3] | ((uint16_t)p[4] << 8));

        /* Same axis transforms the single-touch path applies. */
#if CONFIG_LV_GT911_INVERT_X
        x = gt911_status.max_x_coord - x;
#endif
#if CONFIG_LV_GT911_INVERT_Y
        y = gt911_status.max_y_coord - y;
#endif
#if CONFIG_LV_GT911_SWAPXY
        int16_t swap_buf = x;
        x = y;
        y = swap_buf;
#endif

        out->points[i].track_id = p[0];
        out->points[i].x = (uint16_t)x;
        out->points[i].y = (uint16_t)y;
        out->points[i].size = (uint16_t)((uint16_t)p[5] | ((uint16_t)p[6] << 8));
    }
    out->count = count;

    /* Release the buffer so the controller can latch the next sample. */
    gt911_i2c_write8(gt911_status.i2c_dev_addr, GT911_STATUS_REG, 0x00);

    /* Publish for observers (they must not poll I2C themselves). */
    taskENTER_CRITICAL(&s_sample_mux);
    s_last_sample = *out;
    s_sample_valid = true;
    taskEXIT_CRITICAL(&s_sample_mux);

    return ESP_OK;
}

/**
  * @brief  Get the touch screen X and Y positions values. Ignores multi touch
  * @param  drv:
  * @param  data: Store data here
  * @retval Always false
  *
  * Built on gt911_read_points() so there is exactly one place talking to the
  * controller. The GT911 status register is read-to-clear, so having more than
  * one reader steals samples from the others; everything now funnels through
  * this single-point wrapper.
  */
bool gt911_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    static int16_t last_x = 0;  // 12bit pixel value
    static int16_t last_y = 0;  // 12bit pixel value

    gt911_touch_data_t td;
    if (gt911_read_points(&td) != ESP_OK || td.count == 0) {
        data->point.x = last_x;
        data->point.y = last_y;
        data->state = LV_INDEV_STATE_REL;
        return false;
    }

    /* Single-point widgets only get one coordinate pair, so use the first
     * reported point. Multi-finger UIs read gt911_read_points() directly. */
    last_x = td.points[0].x;
    last_y = td.points[0].y;

    data->point.x = last_x;
    data->point.y = last_y;
    data->state = LV_INDEV_STATE_PR;

    if (td.count > 1) {
        ESP_LOGD(TAG, "%u points down, tracking point 0 at %d,%d",
                 td.count, last_x, last_y);
    } else {
        ESP_LOGI(TAG, "X=%u Y=%u", data->point.x, data->point.y);
    }
    return false;
}
