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

/* 底层 I2C 访问的前置声明。
 * 诊断/复位等辅助函数放在文件前部，而这两个函数定义在后部，所以需要先声明，
 * 否则编译器会把调用当成隐式声明（C99 起是错误）。 */
esp_err_t gt911_i2c_read(uint8_t slave_addr, uint16_t register_addr,
                         uint8_t *data_buf, uint8_t len);
esp_err_t gt911_i2c_write8(uint8_t slave_addr, uint16_t register_addr,
                           uint8_t data);
esp_err_t gt911_read_points(gt911_touch_data_t *out);

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

/* ---------------------------------------------------------------------------
 * I2C 序列化。
 *
 * GT911 的状态寄存器是「读后清零」的：谁先读到 buffer-ready，谁就把标志清掉，
 * 另一个人这一轮就看不到数据。所以全局只能有一个读取者，且它的调用必须串行。
 *
 * 本项目的分配：
 *   - 宏键盘界面（kb_keypad.c 的触摸任务）是唯一读取者，调用
 *     gt911_sample_now()。它不挂在 LVGL 的 indev 上，因为 LVGL v8 的单点
 *     模型无法表达多指同时按下。
 *   - 若将来重新启用 LVGL indev（gt911_read()），它同样会走这把锁，两者
 *     互斥，不会互相偷采样。
 * ------------------------------------------------------------------------- */
static SemaphoreHandle_t s_i2c_lock = NULL;

void gt911_i2c_lock_init(void)
{
    if (s_i2c_lock == NULL) {
        s_i2c_lock = xSemaphoreCreateMutex();
    }
}

esp_err_t gt911_sample_now(gt911_touch_data_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_i2c_lock == NULL) {
        gt911_i2c_lock_init();
    }

    if (s_i2c_lock != NULL &&
        xSemaphoreTake(s_i2c_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = gt911_read_points(out);

    if (s_i2c_lock != NULL) {
        xSemaphoreGive(s_i2c_lock);
    }
    return ret;
}

/* ---------------------------------------------------------------------------
 * 原始诊断：直接读状态寄存器并打印，用于排查「触摸没反应」。
 *
 * 状态寄存器 0x814E 的 bit7 是 buffer-ready，低 4 位是触点数量。
 * 如果这里一直是 0，说明控制器压根没检测到触摸（硬件/供电/中断配置问题）；
 * 如果非 0 而 gt911_read_points 拿不到，则是读取顺序或清除时机的问题。
 * ------------------------------------------------------------------------- */
uint8_t gt911_debug_status(void)
{
    uint8_t status = 0;
    if (!gt911_status.inited) {
        return 0xFF;   /* 未初始化，用 0xFF 区分 */
    }
    if (gt911_i2c_read(gt911_status.i2c_dev_addr, GT911_STATUS_REG, &status, 1) != ESP_OK) {
        return 0xFE;   /* I2C 失败 */
    }
    return status;
}

/* ---------------------------------------------------------------------------
 * 深度诊断：读出一组关键寄存器，判断芯片处于什么状态。
 *
 * 0x8040  Command/Config 区（不同固件版本定义略有差异，但可用于观察是否可读）
 * 0x8047  Config version
 * 0x8140  Product ID
 * 0x8146  X resolution
 * 0x8148  Y resolution
 * 0x814A  Vendor ID
 * 0x814E  Status
 * 0x8150  Point 1 X
 *
 * 若这些都可读且数值合理，说明 I2C 与芯片供电正常，问题集中在触摸检测侧
 * （例如固件被复位后的初始化未完成、或 INT/RST 状态不对）。
 * ------------------------------------------------------------------------- */
void gt911_debug_dump(void)
{
    if (!gt911_status.inited) {
        ESP_LOGW(TAG, "debug_dump: not initialised");
        return;
    }

    static const uint16_t regs[] = {
        0x8040, 0x8047, 0x8140, 0x8146, 0x8148, 0x814A, 0x814E, 0x8150,
    };

    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        uint8_t v = 0;
        esp_err_t r = gt911_i2c_read(gt911_status.i2c_dev_addr, regs[i], &v, 1);
        if (r == ESP_OK) {
            ESP_LOGI(TAG, "reg[0x%04X] = 0x%02X", regs[i], v);
        } else {
            ESP_LOGE(TAG, "reg[0x%04X] read failed: %s", regs[i], esp_err_to_name(r));
        }
    }
}

/* ---------------------------------------------------------------------------
 * 原始触点数据转储（只在诊断时调用）。
 *
 * 直接打印 0x814E 起 44 字节的原始内容，用来判断：
 *   - 状态字节低 4 位到底是不是 0
 *   - 坐标区是否真的有数据（哪怕状态位没标记）
 * 这比看转换后的 count 更能反映控制器真实行为。
 *
 * 注意：读 0x814E 会触发 GT911 的状态确认，调用本函数会消耗掉一次触摸事件，
 * 因此只在排查阶段用。
 * ------------------------------------------------------------------------- */
void gt911_debug_raw_points(void)
{
    if (!gt911_status.inited) {
        ESP_LOGW(TAG, "raw dump: not initialised");
        return;
    }

    /* 从状态寄存器开始一次性读 44 字节：
     * 0x814E 状态 + 0x814F..0x8174 五个触点 */
    uint8_t buf[44] = {0};
    esp_err_t r = gt911_i2c_read(gt911_status.i2c_dev_addr, GT911_STATUS_REG,
                                 buf, sizeof(buf));
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "raw dump read failed: %s", esp_err_to_name(r));
        return;
    }

    ESP_LOGI(TAG, "raw[0x814E..] status=0x%02X (buf=%d cnt=%d)",
             buf[0], (buf[0] & 0x80) ? 1 : 0, buf[0] & 0x0F);

    char line[128];
    for (int i = 1; i < 44; i += 7) {
        int n = 0;
        line[0] = '\0';
        for (int j = i; j < i + 7 && j < 44; j++) {
            n += snprintf(line + n, sizeof(line) - n, "%02X ", buf[j]);
        }
        ESP_LOGI(TAG, "  [0x%04X] %s", GT911_STATUS_REG + i, line);
    }
}

/* ---------------------------------------------------------------------------
 * 软件复位并重新初始化 GT911。
 *
 * 有些批次/固件在硬件复位后若没有及时完成内部初始化，会停留在不产生触摸
 * 事件的状态（状态寄存器恒 0）。发一次软件复位（0x8040 写 0x02）并等待
 * 固件重新就绪，通常能恢复。
 * ------------------------------------------------------------------------- */
esp_err_t gt911_soft_reset(void)
{
    if (!gt911_status.inited) {
        return ESP_ERR_INVALID_STATE;
    }

    /* GT911 命令寄存器 0x8040：写 0x02 = 软复位 */
    esp_err_t ret = gt911_i2c_write8(gt911_status.i2c_dev_addr, 0x8040, 0x02);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "soft reset write failed: %s", esp_err_to_name(ret));
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(120));

    /* 复位后控制器的状态寄存器应可正常读写 */
    uint8_t st = 0;
    gt911_i2c_read(gt911_status.i2c_dev_addr, GT911_STATUS_REG, &st, 1);
    ESP_LOGI(TAG, "soft reset done, status reg = 0x%02X", st);

    return ESP_OK;
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

    /* Bit 7 = buffer ready；低 4 位 = 本次上报的触点数。
     *
     * 这里有个容易写错的地方：buffer-ready 置位但低 4 位为 0，表示控制器
     * 「已检测到触摸、但坐标尚未算完」。此时绝不能写 0 清标志——GT911 的
     * 状态寄存器是读后由主机写 0 确认的，提前清掉会让它一直无法完成这次
     * 报告，表现为状态恒 0、触摸完全失灵。
     *
     * 因此只在真正读到有效坐标之后才清标志。 */
    uint8_t count = status_reg & GT911_STATUS_REG_PT_MASK;
    if ((status_reg & GT911_STATUS_REG_BUF) == 0 || count == 0) {
        /* 没有可用的新数据。不写 0，把清标志的机会留给下一个完整周期。
         * 仅当明确「无触摸」时才清（此时低 4 位为 0、bit7 也为 0 是常态）。 */
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
  * Built on gt911_sample_now() so the I2C transaction is serialised with any
  * other reader. The GT911 status register is read-to-clear, so concurrent
  * readers would steal samples from each other.
  */
bool gt911_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    static int16_t last_x = 0;  // 12bit pixel value
    static int16_t last_y = 0;  // 12bit pixel value

    gt911_touch_data_t td;
    if (gt911_sample_now(&td) != ESP_OK || td.count == 0) {
        data->point.x = last_x;
        data->point.y = last_y;
        data->state = LV_INDEV_STATE_REL;
        return false;
    }

    /* Single-point widgets only get one coordinate pair, so use the first
     * reported point. Multi-finger UIs read the full set instead. */
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
