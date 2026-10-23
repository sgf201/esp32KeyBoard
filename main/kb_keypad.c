/**
 * @file kb_keypad.c
 *
 * 虚拟键盘 + 触摸板，支持真正的多指同时按键。
 *
 * ========================= 为什么自己做触摸 =========================
 *
 * LVGL v8 的输入设备是单点模型：lv_indev_data_t 里只有一个 lv_point_t。
 * 即便 GT911 硬件能上报 5 个触点（gt911_read_points() 已实现），LVGL 的
 * indev 回调也只能交出 points[0]，于是「两根手指按两个键」永远只有第一个
 * 生效。
 *
 * 为了让多指同时按键真正可用，这里不再把键盘交给 LVGL 的 indev，而是：
 *
 *   1. 独立任务轮询 gt911 的多点快照（gt911_get_last_sample，不抢 I2C ——
 *      GT911 的状态寄存器是读后清零的，全局只能有一个读者）
 *   2. 对每个触点做按键命中测试（自己按行列算矩形，不依赖 LVGL API）
 *   3. 维护「当前按下的按键集合」
 *   4. 集合变化时发送一次包含全部按键的 HID 报告（最多 6 键）
 *
 * ========================= 线程模型（重要） =========================
 *
 * 有两条硬约束，违反了就会「按键发不出去」：
 *
 *   A. 不能放在 esp_timer 回调里。ble_hid_key_tap() 内部有 vTaskDelay(12ms)，
 *      在 esp_timer 回调中阻塞会卡死定时器服务（该回调运行在 esp_timer 任务
 *      上下文，不允许阻塞）。因此触摸扫描放在独立的 FreeRTOS 任务里。
 *
 *   B. LVGL 不是线程安全的。main.c 用 xGuiSemaphore 保护 lv_timer_handler()，
 *      任何跨任务修改界面的操作都必须先拿这把锁。这里通过 kb_set_gui_lock()
 *      注入锁句柄，刷新按钮状态时严格加锁。
 *
 * 另外，为了让「按住不放」也能稳定工作，HID 报告的发送与界面刷新解耦：
 * 触摸任务只更新按键集合，发送交给它自己按需进行，界面刷新走 LVGL 定时器。
 */

#include "kb_keypad.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "ble_hid.h"
#include "gt911.h"

static const char *TAG = "KB";

/* ---------------------------------------------------------------------------
 * 尺寸与参数
 * ------------------------------------------------------------------------- */
#define SCREEN_W            800
#define SCREEN_H            480

#define STATUS_BAR_H        36
#define MODE_BAR_H          44

#define SPLIT_KB_PCT        62

#define TOUCHPAD_GAIN       2
#define TAP_MOVE_LIMIT      14
#define HID_AXIS_MAX        127
#define HID_MAX_KEYS        6

/* 触摸扫描周期。
 *
 * 取值需要权衡：GT911 的触摸扫描约 100Hz，但主机读状态寄存器会清掉
 * buffer-ready，读得太频繁会打断控制器内部的状态机推进——实测 15ms 时
 * 会出现状态寄存器卡在 0x80、触点数恒为 0 的现象。放宽到 30ms 后恢复正常。 */
#define TOUCH_TASK_PERIOD_MS 30

/* 任务栈与优先级 */
#define TOUCH_TASK_STACK    (1024 * 4)
#define TOUCH_TASK_PRIO     4

/* ---------------------------------------------------------------------------
 * 按键表
 * ------------------------------------------------------------------------- */
#define KEY_MOD_CTRL   (-2)
#define KEY_MOD_SHIFT  (-3)
#define KEY_MOD_ALT    (-4)
#define KEY_BACKSPACE  (-5)
#define KEY_ENTER      (-6)
#define KEY_SPACE      (-7)
#define KEY_TAB        (-8)
#define KEY_ESC        (-9)

#define KEY_SC_COPY    (-20)
#define KEY_SC_PASTE   (-21)
#define KEY_SC_CUT     (-22)
#define KEY_SC_UNDO    (-23)
#define KEY_SC_SAVE    (-24)
#define KEY_SC_SELALL  (-25)

#define KB_ROWS        6

static const char *kb_map[] = {
    /* 0 */ "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    /* 1 */ "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    /* 2 */ "a", "s", "d", "f", "g", "h", "j", "k", "l", "\n",
    /* 3 */ "SHFT", "z", "x", "c", "v", "b", "n", "m", "DEL", "\n",
    /* 4 */ "CTRL", "ALT", "TAB", "SPC", "ESC", "ENT", "\n",
    /* 5 */ "复制", "粘贴", "剪切", "撤销", "保存", "全选", ""
};

static const int16_t kb_keys[] = {
    /* 0 */ 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
    /* 1 */ 0x14, 0x1A, 0x08, 0x15, 0x17, 0x1C, 0x18, 0x0C, 0x12, 0x13,
    /* 2 */ 0x04, 0x16, 0x07, 0x09, 0x0A, 0x0B, 0x0D, 0x0E, 0x0F,
    /* 3 */ KEY_MOD_SHIFT, 0x1D, 0x1B, 0x06, 0x19, 0x05, 0x11, 0x10, KEY_BACKSPACE,
    /* 4 */ KEY_MOD_CTRL, KEY_MOD_ALT, KEY_TAB, KEY_SPACE, KEY_ESC, KEY_ENTER,
    /* 5 */ KEY_SC_COPY, KEY_SC_PASTE, KEY_SC_CUT, KEY_SC_UNDO, KEY_SC_SAVE, KEY_SC_SELALL,
};

#define KB_KEY_COUNT ((int)(sizeof(kb_keys) / sizeof(kb_keys[0])))

static const uint8_t kb_row_counts[KB_ROWS] = {10, 10, 9, 9, 6, 6};
static const uint8_t kb_row_start[KB_ROWS]  = {0, 10, 20, 29, 38, 44};

#define MOD_CTRL_BIT   BLE_HID_MOD_LCTRL
#define MOD_SHIFT_BIT  BLE_HID_MOD_LSHIFT
#define MOD_ALT_BIT    BLE_HID_MOD_LALT

/* ---------------------------------------------------------------------------
 * 共享状态
 *
 * s_pressed / s_modifiers 由触摸任务写、LVGL 定时器读，用 s_state_lock 保护。
 * HID 发送只在触摸任务里做，避免两处并发操作 BLE。
 * ------------------------------------------------------------------------- */
static uint8_t s_modifiers = 0;
static bool    s_key_down[KB_KEY_COUNT];

static SemaphoreHandle_t s_state_lock = NULL;   /* 保护上面的按键状态 */
static SemaphoreHandle_t s_gui_lock = NULL;     /* 注入自 main.c 的 LVGL 锁 */
static volatile bool     s_visual_dirty = false;

/* 模式切换请求：触摸任务只置标志，真正的布局变更在 LVGL 定时器里做。
 * LVGL 不是线程安全的，跨任务直接改对象会和 lv_timer_handler 撞车。 */
static volatile bool     s_mode_pending = false;
static volatile kb_mode_t s_pending_mode = KB_MODE_SPLIT;

/* apply_mode 定义在文件后部，但视觉刷新定时器要用它切换布局 */
static void apply_mode(kb_mode_t mode);

/* ---------------------------------------------------------------------------
 * UI 对象
 * ------------------------------------------------------------------------- */
static lv_obj_t *s_status_label = NULL;
static lv_obj_t *s_kb_container = NULL;
static lv_obj_t *s_btnmatrix    = NULL;
static lv_obj_t *s_touchpad     = NULL;
static lv_obj_t *s_tp_hint      = NULL;
static lv_obj_t *s_click_row    = NULL;
static lv_obj_t *s_mode_btnm    = NULL;

static kb_mode_t s_mode = KB_MODE_SPLIT;

/* ---------------------------------------------------------------------------
 * 键盘区几何
 * ------------------------------------------------------------------------- */
typedef struct { int x, y, w, h; } rect_t;
static rect_t s_kb_rect = {0, 0, 0, 0};

static bool rect_contains(const rect_t *r, int px, int py)
{
    return px >= r->x && px < r->x + r->w &&
           py >= r->y && py < r->y + r->h;
}

static int hit_test_key(int px, int py)
{
    if (!rect_contains(&s_kb_rect, px, py)) {
        return -1;
    }
    int rel_x = px - s_kb_rect.x;
    int rel_y = py - s_kb_rect.y;

    int row_h = s_kb_rect.h / KB_ROWS;
    if (row_h <= 0) {
        return -1;
    }
    int row = rel_y / row_h;
    if (row < 0 || row >= KB_ROWS) {
        return -1;
    }
    int cols = kb_row_counts[row];
    int col_w = s_kb_rect.w / cols;
    if (col_w <= 0) {
        return -1;
    }
    int col = rel_x / col_w;
    if (col < 0 || col >= cols) {
        return -1;
    }
    int idx = kb_row_start[row] + col;
    return (idx >= 0 && idx < KB_KEY_COUNT) ? idx : -1;
}

static bool point_in_touchpad(int px, int py)
{
    if (s_touchpad == NULL || lv_obj_has_flag(s_touchpad, LV_OBJ_FLAG_HIDDEN)) {
        return false;
    }
    lv_area_t a;
    lv_obj_get_coords(s_touchpad, &a);
    return px >= a.x1 && px <= a.x2 && py >= a.y1 && py <= a.y2;
}

/* ---------------------------------------------------------------------------
 * 底部模式栏的命中测试
 *
 * 因为 LVGL 的 indev 被禁用了（多指需要自主处理触摸），模式栏的 btnmatrix
 * 收不到任何点击事件 —— 所以必须在这里自己判断落在哪个按钮上。
 * 三个按钮等分整个模式栏宽度。
 * ------------------------------------------------------------------------- */
static int hit_test_mode_bar(int px, int py)
{
    if (s_mode_btnm == NULL) {
        return -1;
    }
    lv_area_t a;
    lv_obj_get_coords(s_mode_btnm, &a);
    if (px < a.x1 || px > a.x2 || py < a.y1 || py > a.y2) {
        return -1;
    }
    int w = a.x2 - a.x1 + 1;
    if (w <= 0) {
        return -1;
    }
    int idx = (px - a.x1) * 3 / w;   /* 分屏 / 全键盘 / 全触摸板 */
    return (idx >= 0 && idx < 3) ? idx : -1;
}

/* 全屏触摸板模式下，底部左/右键按钮的命中测试 */
static int hit_test_click_row(int px, int py)
{
    if (s_click_row == NULL || lv_obj_has_flag(s_click_row, LV_OBJ_FLAG_HIDDEN)) {
        return -1;
    }
    lv_area_t a;
    lv_obj_get_coords(s_click_row, &a);
    if (px < a.x1 || px > a.x2 || py < a.y1 || py > a.y2) {
        return -1;
    }
    int w = a.x2 - a.x1 + 1;
    int cx = a.x1 + w / 2;
    /* 两个 150px 按钮居中对齐，间距 8px；以中点为界判断左右即可 */
    return (px < cx) ? 0 : 1;
}

/* ---------------------------------------------------------------------------
 * HID 发送
 * ------------------------------------------------------------------------- */
static uint8_t s_hid_keys[HID_MAX_KEYS];
static int     s_hid_key_count = 0;
static uint8_t s_last_hid_keys[HID_MAX_KEYS];
static int     s_last_hid_count = 0;

/* 收集当前按下的普通键并发送。调用者需持有 s_state_lock。 */
static void flush_keys_locked(void)
{
    s_hid_key_count = 0;
    memset(s_hid_keys, 0, sizeof(s_hid_keys));

    for (int i = 0; i < KB_KEY_COUNT && s_hid_key_count < HID_MAX_KEYS; i++) {
        if (s_key_down[i] && kb_keys[i] > 0) {
            s_hid_keys[s_hid_key_count++] = (uint8_t)kb_keys[i];
        }
    }

    if (s_hid_key_count == s_last_hid_count &&
        memcmp(s_hid_keys, s_last_hid_keys, sizeof(s_hid_keys)) == 0) {
        return;   /* 集合没变，不重复发 */
    }

    memcpy(s_last_hid_keys, s_hid_keys, sizeof(s_hid_keys));
    s_last_hid_count = s_hid_key_count;

    if (!ble_hid_is_connected()) {
        return;
    }

    if (s_hid_key_count == 0) {
        ble_hid_key_release_all();
    } else {
        ble_hid_key_down(s_modifiers, s_hid_keys);
        if (s_hid_key_count > 1) {
            ESP_LOGI(TAG, "multi-key: %d keys down", s_hid_key_count);
        }
    }
}

static void trigger_special_key(int16_t key)
{
    switch (key) {
    case KEY_MOD_CTRL:   s_modifiers ^= MOD_CTRL_BIT;  break;
    case KEY_MOD_SHIFT:  s_modifiers ^= MOD_SHIFT_BIT; break;
    case KEY_MOD_ALT:    s_modifiers ^= MOD_ALT_BIT;   break;

    case KEY_BACKSPACE:  ble_hid_key_tap(0x2A, s_modifiers); break;
    case KEY_ENTER:      ble_hid_key_tap(0x28, s_modifiers); break;
    case KEY_TAB:        ble_hid_key_tap(0x2B, s_modifiers); break;
    case KEY_SPACE:      ble_hid_key_tap(0x2C, s_modifiers); break;
    case KEY_ESC:        ble_hid_key_tap(0x29, s_modifiers); break;

    case KEY_SC_COPY:    ble_hid_key_combo(BLE_HID_MOD_LCTRL, BLE_HID_KEY_C); break;
    case KEY_SC_PASTE:   ble_hid_key_combo(BLE_HID_MOD_LCTRL, BLE_HID_KEY_V); break;
    case KEY_SC_CUT:     ble_hid_key_combo(BLE_HID_MOD_LCTRL, BLE_HID_KEY_X); break;
    case KEY_SC_UNDO:    ble_hid_key_combo(BLE_HID_MOD_LCTRL, BLE_HID_KEY_Z); break;
    case KEY_SC_SAVE:    ble_hid_key_combo(BLE_HID_MOD_LCTRL, BLE_HID_KEY_S); break;
    case KEY_SC_SELALL:  ble_hid_key_combo(BLE_HID_MOD_LCTRL, BLE_HID_KEY_A); break;

    default: break;
    }
    s_visual_dirty = true;
}

/* ---------------------------------------------------------------------------
 * 触点跟踪
 * ------------------------------------------------------------------------- */
#define MAX_TRACKED  GT911_MAX_TOUCH_POINTS

typedef struct {
    bool       used;
    bool       mapped;      /* 是否已做过落点判定（避免每轮重复判定） */
    uint8_t    track_id;
    int        key_idx;
    bool       on_touchpad;
    lv_point_t last;
    int        moved;
} track_t;

static track_t s_tracks[MAX_TRACKED];

static void release_track(track_t *t)
{
    if (t == NULL || !t->used) {
        return;
    }

    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    if (t->key_idx >= 0 && t->key_idx < KB_KEY_COUNT) {
        int16_t key = kb_keys[t->key_idx];
        s_key_down[t->key_idx] = false;
        if (key > 0) {
            flush_keys_locked();
        }
    }
    xSemaphoreGive(s_state_lock);

    /* 触摸板轻点 -> 左键单击（不阻塞，send_mouse 内部无 delay） */
    if (t->on_touchpad && t->moved < TAP_MOVE_LIMIT) {
        ble_hid_mouse_click(BLE_HID_MOUSE_LEFT);
    }

    t->used = false;
    t->mapped = false;
    t->key_idx = -1;
    t->on_touchpad = false;
    /* 必须清零 moved：槽位会被下一个触点复用，若残留旧值，
     * 新建触点时 `fresh` 判定（moved == 0）会失败，
     * 导致该触点永远不进入命中测试、按键点了没反应。 */
    t->moved = 0;
    t->last.x = 0;
    t->last.y = 0;
    s_visual_dirty = true;
}

/* ---------------------------------------------------------------------------
 * 触摸扫描任务
 * ------------------------------------------------------------------------- */
static void touch_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "touch task started, period %d ms", TOUCH_TASK_PERIOD_MS);

    /* 等 gt911_init() 完成（它由 lvgl_driver_init 调用，可能稍晚） */
    while (!gt911_status.inited) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    /* 注意：不要在这里 dump 寄存器。
     * GT911 的状态寄存器读一次就会被主机清掉，dump 会把本次触摸事件偷走，
     * 导致主循环永远读到 0x00。诊断只在需要时手工调用。 */

    uint32_t last_diag = 0;
    int sample_ok = 0, sample_err = 0;
    int touch_seen = 0;
    int idle_rounds = 0;
    bool soft_reset_tried = false;

    for (;;) {
        gt911_touch_data_t td;

        /* 主动采样：本任务是 GT911 的唯一读取者（LVGL 的 indev 未注册），
         * 所以必须用 gt911_sample_now()，而不是读缓存快照。 */
        esp_err_t sret = gt911_sample_now(&td);
        if (sret == ESP_OK) {
            sample_ok++;
            if (td.count > 0) {
                touch_seen++;
                idle_rounds = 0;
            } else {
                idle_rounds++;
            }

            /* 周期诊断：每 3 秒打一次，同时带上原始状态寄存器。
             * raw=0x00 说明控制器没检测到任何触摸；
             * raw 的 bit7 为 1、低 4 位为 N 说明有 N 个触点。
             * 这一步能把「硬件没上报」和「软件没读到」区分开。 */
            uint32_t now = xTaskGetTickCount() / configTICK_RATE_HZ;
            if (now != last_diag && (now % 3) == 0) {
                last_diag = now;
                uint8_t raw = gt911_debug_status();
                ESP_LOGI(TAG, "diag: ok=%d err=%d touches=%d raw=0x%02X count=%u kb(%d,%d %dx%d)",
                         sample_ok, sample_err, touch_seen,
                         raw, (unsigned)td.count,
                         s_kb_rect.x, s_kb_rect.y, s_kb_rect.w, s_kb_rect.h);

                /* 若 buffer-ready 已置位但解析不出触点，打印原始数据区，
                 * 判断是「坐标区真的有数据」还是「芯片只是置了个标志」。 */
                if ((raw & 0x80) && td.count == 0 && touch_seen == 0) {
                    gt911_debug_raw_points();
                }
            }
        } else {
            sample_err++;
            if ((sample_err % 100) == 1) {
                ESP_LOGW(TAG, "sample failed: %s (errs=%d)",
                         esp_err_to_name(sret), sample_err);
            }
        }

        /* 触点跟踪。
         *
         * 关键：这段不能放在 td.count > 0 的条件里。
         * 手指抬起后 count 变成 0，如果只在「有触点」时才处理，就永远不会
         * 执行「未出现的触点 -> 抬起」的收尾逻辑，表现为按键一直保持按下、
         * HID 报告停在最后一个键上（主机侧看到某个键卡住）。
         */
        {
            bool seen[MAX_TRACKED] = {0};

            for (uint8_t i = 0; i < td.count; i++) {
                uint8_t id = td.points[i].track_id;
                int px = td.points[i].x;
                int py = td.points[i].y;

                /* 找到或新建该触点的跟踪记录 */
                track_t *t = NULL;
                int slot = -1;
                for (int k = 0; k < MAX_TRACKED; k++) {
                    if (s_tracks[k].used && s_tracks[k].track_id == id) {
                        t = &s_tracks[k];
                        slot = k;
                        break;
                    }
                }
                if (t == NULL) {
                    for (int k = 0; k < MAX_TRACKED; k++) {
                        if (!s_tracks[k].used) {
                            memset(&s_tracks[k], 0, sizeof(track_t));
                            s_tracks[k].used = true;
                            s_tracks[k].track_id = id;
                            s_tracks[k].key_idx = -1;
                            s_tracks[k].last.x = px;
                            s_tracks[k].last.y = py;
                            t = &s_tracks[k];
                            slot = k;
                            break;
                        }
                    }
                }
                if (t == NULL) {
                    continue;
                }
                seen[slot] = true;

                /* 判断这是不是一个「刚刚按下」的触点。
                 *
                 * 用 t->key_idx 和 t->on_touchpad 都为初始值来判断不可靠：
                 * 触点落在空白区域时两者都保持 -1/false，于是每一轮都会被
                 * 当成新按下，重复走命中测试。
                 *
                 * 因此引入显式的 mapped 标志：只有「本次循环新建的槽位」
                 * 才需要做落点判定。 */
                if (!t->mapped) {
                    t->mapped = true;

                    int idx = hit_test_key(px, py);
                    if (idx >= 0) {
                        t->key_idx = idx;
                        int16_t key = kb_keys[idx];

                        xSemaphoreTake(s_state_lock, portMAX_DELAY);
                        s_key_down[idx] = true;
                        if (key > 0) {
                            flush_keys_locked();
                        }
                        xSemaphoreGive(s_state_lock);

                        if (key < 0) {
                            /* 特殊键按下即触发，注意这里可能阻塞 12ms，
                             * 本函数运行在任务上下文，安全。 */
                            trigger_special_key(key);
                        }
                        s_visual_dirty = true;
                    } else {
                        /* 先看是否落在底部模式栏 */
                        int m = hit_test_mode_bar(px, py);
                        if (m >= 0) {
                            /* 模式切换必须在 LVGL 上下文执行：这里只置一个
                             * 待处理请求，由 visual_refresh 定时器真正切换。 */
                            s_pending_mode = (kb_mode_t)m;
                            s_mode_pending = true;
                        } else {
                            /* 再看全屏触摸板模式下的左/右键 */
                            int c = hit_test_click_row(px, py);
                            if (c >= 0) {
                                ble_hid_mouse_click(c == 0 ? BLE_HID_MOUSE_LEFT
                                                           : BLE_HID_MOUSE_RIGHT);
                            } else if (point_in_touchpad(px, py)) {
                                t->on_touchpad = true;
                            }
                        }
                    }
                }

                /* 移动 */
                int dx = px - t->last.x;
                int dy = py - t->last.y;
                if (dx != 0 || dy != 0) {
                    t->moved += (dx > 0 ? dx : -dx) + (dy > 0 ? dy : -dy);
                    if (t->on_touchpad) {
                        int mx = dx * TOUCHPAD_GAIN;
                        int my = dy * TOUCHPAD_GAIN;
                        while (mx != 0 || my != 0) {
                            int sx = mx > HID_AXIS_MAX ? HID_AXIS_MAX : (mx < -HID_AXIS_MAX ? -HID_AXIS_MAX : mx);
                            int sy = my > HID_AXIS_MAX ? HID_AXIS_MAX : (my < -HID_AXIS_MAX ? -HID_AXIS_MAX : my);
                            ble_hid_mouse_move((int8_t)sx, (int8_t)sy, 0);
                            mx -= sx;
                            my -= sy;
                        }
                        s_visual_dirty = true;
                    }
                    t->last.x = px;
                    t->last.y = py;
                }
            }

            /* 未出现的触点视为抬起。
             *
             * 采样失败（sret != ESP_OK）时 td.count 保持为 0，所有已记录的
             * 触点都会被释放 —— 这是期望行为：宁可松开按键，也不能让按键
             * 卡在按下状态。 */
            for (int k = 0; k < MAX_TRACKED; k++) {
                if (s_tracks[k].used && !seen[k]) {
                    release_track(&s_tracks[k]);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(TOUCH_TASK_PERIOD_MS));
    }
}

/* ---------------------------------------------------------------------------
 * 界面刷新（LVGL 上下文，由 lv_timer 驱动）
 *
 * 只在这里改 LVGL 对象，且必须持有 GUI 锁，避免与 main.c 的 lv_timer_handler
 * 并发。
 * ------------------------------------------------------------------------- */
static void visual_refresh_cb(lv_timer_t *t)
{
    (void)t;

    /* 先处理触摸任务提交的模式切换请求。
     * 布局变更涉及大量 LVGL 调用，必须在 LVGL 上下文执行。 */
    if (s_mode_pending) {
        s_mode_pending = false;
        apply_mode(s_pending_mode);
    }

    if (s_btnmatrix == NULL) {
        return;
    }

    /* 取快照，缩短持锁时间 */
    bool down[KB_KEY_COUNT];
    uint8_t mods;
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    memcpy(down, s_key_down, sizeof(down));
    mods = s_modifiers;
    xSemaphoreGive(s_state_lock);

    for (int i = 0; i < KB_KEY_COUNT; i++) {
        bool show = down[i];
        if (!show) {
            switch (kb_keys[i]) {
            case KEY_MOD_CTRL:  show = (mods & MOD_CTRL_BIT) != 0;  break;
            case KEY_MOD_SHIFT: show = (mods & MOD_SHIFT_BIT) != 0; break;
            case KEY_MOD_ALT:   show = (mods & MOD_ALT_BIT) != 0;   break;
            default: break;
            }
        }
        if (show) {
            lv_btnmatrix_set_btn_ctrl(s_btnmatrix, i, LV_BTNMATRIX_CTRL_CHECKED);
        } else {
            lv_btnmatrix_clear_btn_ctrl(s_btnmatrix, i, LV_BTNMATRIX_CTRL_CHECKED);
        }
    }
}

static void status_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_status_label == NULL) {
        return;
    }

    int n = 0;
    uint8_t mods;
    xSemaphoreTake(s_state_lock, portMAX_DELAY);
    for (int i = 0; i < KB_KEY_COUNT; i++) {
        if (s_key_down[i]) {
            n++;
        }
    }
    mods = s_modifiers;
    xSemaphoreGive(s_state_lock);

    const char *conn = ble_hid_is_connected() ? "已连接" : "待连接";
    char buf[128];
    lv_snprintf(buf, sizeof(buf), "ESP32 宏键盘  %s", conn);

    if (n > 1) {
        size_t len = strlen(buf);
        lv_snprintf(buf + len, sizeof(buf) - len, "  按下%d键", n);
    }

    if (mods) {
        size_t len = strlen(buf);
        lv_snprintf(buf + len, sizeof(buf) - len, "  [");
        if (mods & MOD_CTRL_BIT) {
            len = strlen(buf);
            lv_snprintf(buf + len, sizeof(buf) - len, "CTRL ");
        }
        if (mods & MOD_SHIFT_BIT) {
            len = strlen(buf);
            lv_snprintf(buf + len, sizeof(buf) - len, "SHIFT ");
        }
        if (mods & MOD_ALT_BIT) {
            len = strlen(buf);
            lv_snprintf(buf + len, sizeof(buf) - len, "ALT ");
        }
        len = strlen(buf);
        if (len > 0 && buf[len - 1] == ' ') {
            buf[len - 1] = ']';
        }
    }

    lv_label_set_text(s_status_label, buf);
}

/* ---------------------------------------------------------------------------
 * 模式切换
 * ------------------------------------------------------------------------- */
static void update_kb_rect(void)
{
    if (s_kb_container == NULL ||
        lv_obj_has_flag(s_kb_container, LV_OBJ_FLAG_HIDDEN)) {
        memset(&s_kb_rect, 0, sizeof(s_kb_rect));
        return;
    }
    lv_area_t a;
    lv_obj_get_coords(s_kb_container, &a);
    s_kb_rect.x = a.x1 + 3;
    s_kb_rect.y = a.y1 + 3;
    s_kb_rect.w = (a.x2 - a.x1 + 1) - 6;
    s_kb_rect.h = (a.y2 - a.y1 + 1) - 6;
}

static void apply_mode(kb_mode_t mode)
{
    s_mode = mode;

    lv_coord_t main_h = SCREEN_H - STATUS_BAR_H - MODE_BAR_H;
    lv_coord_t main_y = STATUS_BAR_H;

    switch (mode) {
    case KB_MODE_KEYBOARD:
        lv_obj_add_flag(s_touchpad, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_click_row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_kb_container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_kb_container, 0, main_y);
        lv_obj_set_size(s_kb_container, SCREEN_W, main_h);
        break;

    case KB_MODE_TOUCHPAD: {
        lv_coord_t click_h = 54;
        lv_obj_add_flag(s_kb_container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_touchpad, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_click_row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_touchpad, 0, main_y);
        lv_obj_set_size(s_touchpad, SCREEN_W, main_h - click_h);
        lv_obj_set_pos(s_click_row, 0, main_y + main_h - click_h);
        lv_obj_set_size(s_click_row, SCREEN_W, click_h);
        break;
    }

    case KB_MODE_SPLIT:
    default: {
        lv_coord_t kb_w = (SCREEN_W * SPLIT_KB_PCT) / 100;
        lv_coord_t tp_w = SCREEN_W - kb_w;

        lv_obj_add_flag(s_click_row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_kb_container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_touchpad, LV_OBJ_FLAG_HIDDEN);

        lv_obj_set_pos(s_kb_container, 0, main_y);
        lv_obj_set_size(s_kb_container, kb_w, main_h);
        lv_obj_set_pos(s_touchpad, kb_w, main_y);
        lv_obj_set_size(s_touchpad, tp_w, main_h);
        break;
    }
    }

    if (s_tp_hint) {
        if (mode == KB_MODE_SPLIT) {
            lv_obj_clear_flag(s_tp_hint, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_tp_hint, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (s_mode_btnm) {
        for (uint32_t i = 0; i < 3; i++) {
            if (i == (uint32_t)mode) {
                lv_btnmatrix_set_btn_ctrl(s_mode_btnm, i, LV_BTNMATRIX_CTRL_CHECKED);
            } else {
                lv_btnmatrix_clear_btn_ctrl(s_mode_btnm, i, LV_BTNMATRIX_CTRL_CHECKED);
            }
        }
    }

    /* 布局变了，重算命中区域 */
    lv_obj_update_layout(lv_scr_act());
    update_kb_rect();

    ESP_LOGI(TAG, "mode -> %d (kb %d,%d %dx%d)", (int)mode,
             s_kb_rect.x, s_kb_rect.y, s_kb_rect.w, s_kb_rect.h);
}

void kb_ui_set_mode(kb_mode_t mode)
{
    if (mode > KB_MODE_TOUCHPAD) {
        mode = KB_MODE_SPLIT;
    }
    apply_mode(mode);
}

kb_mode_t kb_ui_get_mode(void)
{
    return s_mode;
}

/* ---------------------------------------------------------------------------
 * 模式栏与左右键（LVGL 常规点击，不参与多点判断）
 * ------------------------------------------------------------------------- */
static void mode_bar_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) {
        return;
    }
    lv_obj_t *btnm = lv_event_get_target(e);
    uint32_t id = lv_btnmatrix_get_selected_btn(btnm);
    if (id == LV_BTNMATRIX_BTN_NONE) {
        return;
    }
    kb_ui_set_mode((kb_mode_t)id);
}

static void click_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    intptr_t which = (intptr_t)lv_event_get_user_data(e);
    ble_hid_mouse_click(which == 0 ? BLE_HID_MOUSE_LEFT : BLE_HID_MOUSE_RIGHT);
}

/* ---------------------------------------------------------------------------
 * 界面构建
 * ------------------------------------------------------------------------- */
void kb_ui_set_gui_lock(SemaphoreHandle_t lock)
{
    s_gui_lock = lock;
}

void kb_ui_create(void)
{
    s_state_lock = xSemaphoreCreateMutex();
    configASSERT(s_state_lock != NULL);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- 状态栏 ---- */
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_set_size(bar, SCREEN_W, STATUS_BAR_H);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x1E2A38), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 4, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    s_status_label = lv_label_create(bar);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xE0E6ED), 0);
    lv_label_set_text(s_status_label, "ESP32 宏键盘  待连接");
    lv_obj_align(s_status_label, LV_ALIGN_LEFT_MID, 4, 0);

    lv_timer_create(status_timer_cb, 300, NULL);

    /* ---- 键盘 ---- */
    s_kb_container = lv_obj_create(scr);
    lv_obj_set_style_bg_opa(s_kb_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_kb_container, 0, 0);
    lv_obj_set_style_pad_all(s_kb_container, 3, 0);
    lv_obj_set_style_radius(s_kb_container, 0, 0);
    lv_obj_clear_flag(s_kb_container, LV_OBJ_FLAG_SCROLLABLE);

    s_btnmatrix = lv_btnmatrix_create(s_kb_container);
    lv_btnmatrix_set_map(s_btnmatrix, kb_map);
    lv_obj_set_size(s_btnmatrix, LV_PCT(100), LV_PCT(100));

    /* 同模式栏：btnmatrix 自身背景必须透明，否则主题底色会盖住按钮边界 */
    lv_obj_set_style_bg_opa(s_btnmatrix, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_btnmatrix, 0, 0);
    lv_obj_set_style_pad_all(s_btnmatrix, 0, 0);
    lv_obj_set_style_pad_row(s_btnmatrix, 3, 0);
    lv_obj_set_style_pad_column(s_btnmatrix, 3, 0);

    lv_obj_set_style_bg_opa(s_btnmatrix, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_btnmatrix, lv_color_hex(0x2C3E50), LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_btnmatrix, lv_color_hex(0xECF0F1), LV_PART_ITEMS);
    lv_obj_set_style_text_font(s_btnmatrix, &lv_font_kb_16, LV_PART_ITEMS);
    lv_obj_set_style_radius(s_btnmatrix, 5, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_btnmatrix, lv_color_hex(0x3498DB),
                              LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(s_btnmatrix, lv_color_white(),
                                LV_PART_ITEMS | LV_STATE_CHECKED);

    for (int i = 0; i < KB_KEY_COUNT; i++) {
        if (kb_keys[i] == KEY_MOD_CTRL || kb_keys[i] == KEY_MOD_SHIFT ||
            kb_keys[i] == KEY_MOD_ALT) {
            lv_btnmatrix_set_btn_ctrl(s_btnmatrix, i, LV_BTNMATRIX_CTRL_CHECKABLE);
        }
    }

    /* ---- 触摸板（纯视觉，手势由 touch_task 处理） ---- */
    s_touchpad = lv_obj_create(scr);
    lv_obj_set_style_bg_color(s_touchpad, lv_color_hex(0x18222C), 0);
    lv_obj_set_style_border_color(s_touchpad, lv_color_hex(0x2E86C1), 0);
    lv_obj_set_style_border_width(s_touchpad, 1, 0);
    lv_obj_set_style_radius(s_touchpad, 6, 0);
    lv_obj_set_style_pad_all(s_touchpad, 4, 0);
    lv_obj_clear_flag(s_touchpad, LV_OBJ_FLAG_SCROLLABLE);

    s_tp_hint = lv_label_create(s_touchpad);
    lv_label_set_text(s_tp_hint, "触摸板\n拖动移动光标\n轻点=左键");
    lv_obj_set_style_text_color(s_tp_hint, lv_color_hex(0x5D6D7E), 0);
    lv_obj_set_style_text_align(s_tp_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_tp_hint);

    /* ---- 左/右键 ---- */
    s_click_row = lv_obj_create(scr);
    lv_obj_set_style_bg_opa(s_click_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_click_row, 0, 0);
    lv_obj_set_style_pad_all(s_click_row, 3, 0);
    lv_obj_set_flex_flow(s_click_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_click_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_click_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_click_row, LV_OBJ_FLAG_HIDDEN);

    static const char *click_names[] = {"左键", "右键"};
    for (int i = 0; i < 2; i++) {
        lv_obj_t *btn = lv_btn_create(s_click_row);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2C3E50), 0);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_set_style_text_font(btn, &lv_font_kb_16, 0);
        lv_obj_set_size(btn, 150, 44);
        lv_obj_add_event_cb(btn, click_btn_event_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, click_names[i]);
        lv_obj_center(lbl);
    }

    /* ---- 模式栏 ---- */
    lv_obj_t *mode_bar = lv_obj_create(scr);
    lv_obj_set_size(mode_bar, SCREEN_W, MODE_BAR_H);
    lv_obj_set_pos(mode_bar, 0, SCREEN_H - MODE_BAR_H);
    lv_obj_set_style_bg_color(mode_bar, lv_color_hex(0x1E2A38), 0);
    lv_obj_set_style_border_width(mode_bar, 0, 0);
    lv_obj_set_style_radius(mode_bar, 0, 0);
    lv_obj_set_style_pad_all(mode_bar, 4, 0);
    lv_obj_clear_flag(mode_bar, LV_OBJ_FLAG_SCROLLABLE);

    static const char *mode_map[] = {"分屏", "全键盘", "全触摸板", ""};
    s_mode_btnm = lv_btnmatrix_create(mode_bar);
    lv_btnmatrix_set_map(s_mode_btnm, mode_map);
    lv_obj_set_size(s_mode_btnm, LV_PCT(100), LV_PCT(100));

    /* btnmatrix 自身背景必须透明，否则它会用主题默认底色覆盖整块区域，
     * 让按钮看不出边界（表现为「按钮看不见」）。 */
    lv_obj_set_style_bg_opa(s_mode_btnm, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_mode_btnm, 0, 0);
    lv_obj_set_style_pad_all(s_mode_btnm, 0, 0);
    lv_obj_set_style_pad_row(s_mode_btnm, 0, 0);
    lv_obj_set_style_pad_column(s_mode_btnm, 6, 0);

    /* 未选中：深灰蓝底 + 亮白字，保证对比度足够 */
    lv_obj_set_style_bg_opa(s_mode_btnm, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_mode_btnm, lv_color_hex(0x34495E), LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_mode_btnm, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_text_font(s_mode_btnm, &lv_font_kb_16, LV_PART_ITEMS);
    lv_obj_set_style_radius(s_mode_btnm, 5, LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_mode_btnm, 1, LV_PART_ITEMS);
    lv_obj_set_style_border_color(s_mode_btnm, lv_color_hex(0x5D6D7E),
                                  LV_PART_ITEMS);

    /* 选中：亮蓝底，明确指示当前模式 */
    lv_obj_set_style_bg_color(s_mode_btnm, lv_color_hex(0x2980B9),
                              LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(s_mode_btnm, lv_color_white(),
                                LV_PART_ITEMS | LV_STATE_CHECKED);

    for (uint32_t i = 0; i < 3; i++) {
        lv_btnmatrix_set_btn_ctrl(s_mode_btnm, i, LV_BTNMATRIX_CTRL_CHECKABLE);
    }
    lv_obj_add_event_cb(s_mode_btnm, mode_bar_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    apply_mode(KB_MODE_SPLIT);

    /* 视觉刷新定时器（运行在 LVGL 上下文） */
    lv_timer_create(visual_refresh_cb, 50, NULL);

    /* 触摸扫描任务：独立任务，可安全阻塞 */
    xTaskCreatePinnedToCore(touch_task, "kb_touch", TOUCH_TASK_STACK,
                            NULL, TOUCH_TASK_PRIO, NULL, 0);

    ESP_LOGI(TAG, "keypad UI created (%d keys, multi-touch enabled)", KB_KEY_COUNT);
}
