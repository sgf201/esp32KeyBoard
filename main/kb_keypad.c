/**
 * @file kb_keypad.c
 *
 * 虚拟键盘 + 触摸板界面实现。
 *
 * ============================ 布局 ============================
 *
 * 三种模式，底部栏常驻以便随时切换：
 *
 *   分屏 (SPLIT)          键盘在左 62%，触摸板在右 38%
 *   全键盘 (KEYBOARD)     键盘占满主区域，按键更大
 *   全触摸板 (TOUCHPAD)   触摸板占满主区域，附左右键按钮
 *
 *   +-----------------------------------------------------------+
 *   | 状态条：连接状态 / 锁定中的修饰键                          | 36px
 *   +----------------------------+------------------------------+
 *   |                            |                              |
 *   |  主区域（按模式变化）                                       |
 *   |                            |                              |
 *   +----------------------------+------------------------------+
 *   | 分屏 | 全键盘 | 全触摸板                                    | 44px
 *   +-----------------------------------------------------------+
 *
 * ======================== 手势设计 ========================
 *
 * 「滑动即触摸」——整个屏幕都支持触摸板手势：
 *   - 手指按下后位移小于阈值就松手  -> 当作点击该按键
 *   - 位移超过阈值                  -> 转为拖动，只移动光标，不输入任何字符
 *
 * 这正是手机输入法的做法：想按某个键直接点，想移动光标就在任意位置滑动。
 * 判定推迟到松手时才做，所以滑动过程中不会误触发按键。
 *
 * ======================== 事件模型 ========================
 *
 * 这里有个容易踩的坑：按键矩阵上的事件会通过 LV_OBJ_FLAG_EVENT_BUBBLE
 * 冒泡到根 screen，而根 screen 上有手势回调。如果手势回调在 PRESSED 时
 * 清空「待发送按键」，就会把按钮刚记录下的按键抹掉，表现为按键全部失灵。
 *
 * 因此约定：
 *   - btnmatrix 的 PRESSED 只负责记录 s_pressed_btn
 *   - 根 screen 的手势回调在 PRESSED 时绝不碰 s_pressed_btn
 *   - 只有手势回调在 RELEASED 时，且在「未拖动」前提下才发送并清除
 *   - 一旦判定为拖动，立即作废 s_pressed_btn
 */

#include "kb_keypad.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ble_hid.h"

static const char *TAG = "KB";

/* ---------------------------------------------------------------------------
 * 尺寸与可调参数
 * ------------------------------------------------------------------------- */
#define SCREEN_W            800
#define SCREEN_H            480

#define STATUS_BAR_H        36
#define MODE_BAR_H          44

/* 分屏模式下键盘占左侧的百分比 */
#define SPLIT_KB_PCT        62

/* 触摸板灵敏度：手指每移动 1px，光标移动几个像素。
 * 全屏触摸板面积大，用低一点的增益更跟手。 */
#define TOUCHPAD_GAIN_SPLIT 2
#define TOUCHPAD_GAIN_FULL  2

/* 位移阈值：超过这个累计距离就判定为拖动而非点击 */
#define DRAG_THRESHOLD      16

/* HID 鼠标报告单轴范围 */
#define HID_AXIS_MAX        127

/* 「无待发送按键」的哨兵值 */
#define NO_BUTTON           0xFFFF

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

/* 全键盘模式下复用同一张表，只是按键被拉大 */
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

#define KB_KEY_COUNT (sizeof(kb_keys) / sizeof(kb_keys[0]))

#define MOD_CTRL_BIT   BLE_HID_MOD_LCTRL
#define MOD_SHIFT_BIT  BLE_HID_MOD_LSHIFT
#define MOD_ALT_BIT    BLE_HID_MOD_LALT

static uint8_t s_modifiers = 0;

/* ---------------------------------------------------------------------------
 * UI 对象
 * ------------------------------------------------------------------------- */
static lv_obj_t *s_status_label = NULL;
static lv_obj_t *s_kb_container = NULL;
static lv_obj_t *s_btnmatrix    = NULL;
static lv_obj_t *s_touchpad     = NULL;
static lv_obj_t *s_tp_hint      = NULL;
static lv_obj_t *s_click_row    = NULL;   /* 全屏触摸板模式下的左/右键 */
static lv_obj_t *s_mode_btnm    = NULL;

static kb_mode_t s_mode = KB_MODE_SPLIT;

/* ---------------------------------------------------------------------------
 * 手势状态
 * ------------------------------------------------------------------------- */
typedef struct {
    bool       active;      /* 手指是否按下 */
    bool       dragging;    /* 是否已判定为拖动 */
    lv_point_t origin;      /* 按下点 */
    lv_point_t last;        /* 上次采样点 */
    int        accum;       /* 累计位移 */
} gesture_t;

static gesture_t s_g;

/* 由 btnmatrix 的 PRESSED 写入，由手势回调在松手时消费。
 * 声明为 volatile 是因为它跨越两个回调，且语义上代表「硬件事件」。 */
static volatile uint16_t s_pressed_btn = NO_BUTTON;

/* ---------------------------------------------------------------------------
 * 修饰键高亮
 * ------------------------------------------------------------------------- */
static void refresh_modifier_highlight(void)
{
    if (s_btnmatrix == NULL) {
        return;
    }
    for (uint16_t i = 0; i < KB_KEY_COUNT; i++) {
        uint8_t bit = 0;
        switch (kb_keys[i]) {
        case KEY_MOD_CTRL:  bit = MOD_CTRL_BIT;  break;
        case KEY_MOD_SHIFT: bit = MOD_SHIFT_BIT; break;
        case KEY_MOD_ALT:   bit = MOD_ALT_BIT;   break;
        default: continue;
        }
        if (s_modifiers & bit) {
            lv_btnmatrix_set_btn_ctrl(s_btnmatrix, i, LV_BTNMATRIX_CTRL_CHECKED);
        } else {
            lv_btnmatrix_clear_btn_ctrl(s_btnmatrix, i, LV_BTNMATRIX_CTRL_CHECKED);
        }
    }
}

static void toggle_modifier(uint8_t bit)
{
    if (s_modifiers & bit) {
        s_modifiers &= ~bit;
    } else {
        s_modifiers |= bit;
    }
    ESP_LOGI(TAG, "modifiers -> 0x%02x", s_modifiers);
    refresh_modifier_highlight();
}

/* ---------------------------------------------------------------------------
 * 发送按键
 * ------------------------------------------------------------------------- */
static void send_key_value(int16_t key)
{
    switch (key) {
    case KEY_MOD_CTRL:   toggle_modifier(MOD_CTRL_BIT);  return;
    case KEY_MOD_SHIFT:  toggle_modifier(MOD_SHIFT_BIT); return;
    case KEY_MOD_ALT:    toggle_modifier(MOD_ALT_BIT);   return;

    case KEY_BACKSPACE:  ble_hid_key_tap(0x2A, s_modifiers); break;
    case KEY_ENTER:      ble_hid_key_tap(0x28, s_modifiers); break;
    case KEY_TAB:        ble_hid_key_tap(0x2B, s_modifiers); break;
    case KEY_SPACE:      ble_hid_key_tap(0x2C, s_modifiers); break;
    case KEY_ESC:        ble_hid_key_tap(0x29, s_modifiers); break;

    case KEY_SC_COPY:    ble_hid_key_combo(BLE_HID_MOD_LCTRL, BLE_HID_KEY_C); return;
    case KEY_SC_PASTE:   ble_hid_key_combo(BLE_HID_MOD_LCTRL, BLE_HID_KEY_V); return;
    case KEY_SC_CUT:     ble_hid_key_combo(BLE_HID_MOD_LCTRL, BLE_HID_KEY_X); return;
    case KEY_SC_UNDO:    ble_hid_key_combo(BLE_HID_MOD_LCTRL, BLE_HID_KEY_Z); return;
    case KEY_SC_SAVE:    ble_hid_key_combo(BLE_HID_MOD_LCTRL, BLE_HID_KEY_S); return;
    case KEY_SC_SELALL:  ble_hid_key_combo(BLE_HID_MOD_LCTRL, BLE_HID_KEY_A); return;

    default:
        if (key > 0) {
            ble_hid_key_tap((uint8_t)key, s_modifiers);
        }
        return;
    }
}

/* ---------------------------------------------------------------------------
 * 光标移动
 * ------------------------------------------------------------------------- */
static void move_cursor(int dx, int dy, int gain)
{
    int mx = dx * gain;
    int my = dy * gain;

    while (mx != 0 || my != 0) {
        int sx = mx > HID_AXIS_MAX ? HID_AXIS_MAX : (mx < -HID_AXIS_MAX ? -HID_AXIS_MAX : mx);
        int sy = my > HID_AXIS_MAX ? HID_AXIS_MAX : (my < -HID_AXIS_MAX ? -HID_AXIS_MAX : my);
        ble_hid_mouse_move((int8_t)sx, (int8_t)sy, 0);
        mx -= sx;
        my -= sy;
    }
}

static int current_gain(void)
{
    return (s_mode == KB_MODE_SPLIT) ? TOUCHPAD_GAIN_SPLIT : TOUCHPAD_GAIN_FULL;
}

/* ---------------------------------------------------------------------------
 * 根 screen 上的统一手势处理
 *
 * 注意：这个回调会收到两类事件——直接按在空白处的，以及从按键矩阵冒泡上来的。
 * 它必须能同时正确处理两者，且绝不能抹掉 btnmatrix 记录下的 s_pressed_btn。
 * ------------------------------------------------------------------------- */
static void gesture_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    lv_indev_t *indev = lv_indev_get_act();
    if (indev != NULL && lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) {
        return;
    }

    lv_point_t p = {0, 0};
    if (indev != NULL) {
        lv_indev_get_point(indev, &p);
    }

    switch (code) {
    case LV_EVENT_PRESSED:
        /* 只重置手势几何状态，绝不碰 s_pressed_btn：
         * 该字段可能刚被 btnmatrix 的 PRESSED 写入。 */
        s_g.active = true;
        s_g.dragging = false;
        s_g.accum = 0;
        s_g.origin = p;
        s_g.last = p;
        break;

    case LV_EVENT_PRESSING: {
        if (!s_g.active) {
            break;
        }
        int dx = p.x - s_g.last.x;
        int dy = p.y - s_g.last.y;
        if (dx == 0 && dy == 0) {
            break;
        }
        s_g.accum += (dx > 0 ? dx : -dx) + (dy > 0 ? dy : -dy);

        if (!s_g.dragging && s_g.accum > DRAG_THRESHOLD) {
            s_g.dragging = true;
            /* 一旦开始拖动，这一下就不再是点击：作废待发按键 */
            s_pressed_btn = NO_BUTTON;
            if (s_tp_hint) {
                lv_obj_add_flag(s_tp_hint, LV_OBJ_FLAG_HIDDEN);
            }
            ESP_LOGD(TAG, "drag start");
        }

        if (s_g.dragging) {
            move_cursor(dx, dy, current_gain());
            s_g.last = p;
        }
        break;
    }

    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST: {
        uint16_t btn = s_pressed_btn;
        bool was_drag = s_g.dragging;
        s_g.active = false;
        s_g.dragging = false;
        s_pressed_btn = NO_BUTTON;

        if (code == LV_EVENT_RELEASED && !was_drag && btn != NO_BUTTON &&
            btn < KB_KEY_COUNT) {
            /* 位移很小 -> 真正的点击，这时才发按键 */
            send_key_value(kb_keys[btn]);
        }

        /* 清理按钮的按下态显示 */
        if (s_btnmatrix && btn != NO_BUTTON && btn < KB_KEY_COUNT) {
            if (lv_btnmatrix_get_selected_btn(s_btnmatrix) == btn) {
                lv_btnmatrix_clear_btn_ctrl(s_btnmatrix, btn,
                                            LV_BTNMATRIX_CTRL_CHECKED);
            }
        }
        refresh_modifier_highlight();
        break;
    }

    default:
        break;
    }
}

/* ---------------------------------------------------------------------------
 * 键盘按钮：只记录，不发送
 * ------------------------------------------------------------------------- */
static void btnmatrix_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_PRESSED) {
        return;
    }
    lv_obj_t *btnm = lv_event_get_target(e);
    uint32_t id = lv_btnmatrix_get_selected_btn(btnm);
    if (id != LV_BTNMATRIX_BTN_NONE && id < KB_KEY_COUNT) {
        s_pressed_btn = (uint16_t)id;
    }
}

/* ---------------------------------------------------------------------------
 * 触摸板左右键（仅全屏触摸板模式显示）
 * ------------------------------------------------------------------------- */
static void click_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    intptr_t which = (intptr_t)lv_event_get_user_data(e);
    ble_hid_mouse_click(which == 0 ? BLE_HID_MOUSE_LEFT : BLE_HID_MOUSE_RIGHT);
}

/* ---------------------------------------------------------------------------
 * 模式切换
 * ------------------------------------------------------------------------- */
static void apply_mode(kb_mode_t mode)
{
    s_mode = mode;

    lv_coord_t main_h = SCREEN_H - STATUS_BAR_H - MODE_BAR_H;
    lv_coord_t main_y = STATUS_BAR_H;

    switch (mode) {
    case KB_MODE_KEYBOARD:
        /* 全键盘：键盘占满主区域，按键自然变大 */
        lv_obj_add_flag(s_touchpad, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_click_row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_kb_container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_kb_container, 0, main_y);
        lv_obj_set_size(s_kb_container, SCREEN_W, main_h);
        break;

    case KB_MODE_TOUCHPAD: {
        /* 全触摸板：触摸板占满主区域，底部留一条放左右键 */
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
        /* 分屏：左右分栏 */
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

    /* 触摸板提示文字只在分屏模式给出来（全屏模式空间充足，不需要提示） */
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

    ESP_LOGI(TAG, "mode -> %d", (int)mode);
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

/* ---------------------------------------------------------------------------
 * 状态栏
 * ------------------------------------------------------------------------- */
static void status_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_status_label == NULL) {
        return;
    }

    const char *conn = ble_hid_is_connected() ? "已连接" : "待连接";
    char buf[96];
    lv_snprintf(buf, sizeof(buf), "ESP32 宏键盘  %s", conn);

    if (s_modifiers) {
        size_t n = strlen(buf);
        lv_snprintf(buf + n, sizeof(buf) - n, "  [");
        if (s_modifiers & MOD_CTRL_BIT) {
            n = strlen(buf);
            lv_snprintf(buf + n, sizeof(buf) - n, "CTRL ");
        }
        if (s_modifiers & MOD_SHIFT_BIT) {
            n = strlen(buf);
            lv_snprintf(buf + n, sizeof(buf) - n, "SHIFT ");
        }
        if (s_modifiers & MOD_ALT_BIT) {
            n = strlen(buf);
            lv_snprintf(buf + n, sizeof(buf) - n, "ALT ");
        }
        n = strlen(buf);
        if (n > 0 && buf[n - 1] == ' ') {
            buf[n - 1] = ']';
        }
    }

    lv_label_set_text(s_status_label, buf);
}

/* ---------------------------------------------------------------------------
 * 界面构建
 * ------------------------------------------------------------------------- */
void kb_ui_create(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* 根 screen 承接统一手势 */
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, gesture_event_cb, LV_EVENT_ALL, NULL);

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

    lv_timer_create(status_timer_cb, 500, NULL);

    /* ---- 键盘 ---- */
    s_kb_container = lv_obj_create(scr);
    lv_obj_set_style_bg_opa(s_kb_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_kb_container, 0, 0);
    lv_obj_set_style_pad_all(s_kb_container, 3, 0);
    lv_obj_set_style_radius(s_kb_container, 0, 0);
    lv_obj_clear_flag(s_kb_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_kb_container, LV_OBJ_FLAG_EVENT_BUBBLE);

    s_btnmatrix = lv_btnmatrix_create(s_kb_container);
    lv_btnmatrix_set_map(s_btnmatrix, kb_map);
    lv_obj_set_size(s_btnmatrix, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(s_btnmatrix, 1, 0);
    lv_obj_set_style_pad_row(s_btnmatrix, 3, 0);
    lv_obj_set_style_pad_column(s_btnmatrix, 3, 0);
    lv_obj_set_style_text_font(s_btnmatrix, &lv_font_kb_16, LV_PART_ITEMS);

    lv_obj_set_style_bg_color(s_btnmatrix, lv_color_hex(0x2C3E50), LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_btnmatrix, lv_color_hex(0xECF0F1), LV_PART_ITEMS);
    lv_obj_set_style_radius(s_btnmatrix, 5, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_btnmatrix, lv_color_hex(0x3498DB),
                              LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(s_btnmatrix, lv_color_white(),
                                LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(s_btnmatrix, lv_color_hex(0x1ABC9C),
                              LV_PART_ITEMS | LV_STATE_PRESSED);

    for (uint16_t i = 0; i < KB_KEY_COUNT; i++) {
        if (kb_keys[i] == KEY_MOD_CTRL || kb_keys[i] == KEY_MOD_SHIFT ||
            kb_keys[i] == KEY_MOD_ALT) {
            lv_btnmatrix_set_btn_ctrl(s_btnmatrix, i, LV_BTNMATRIX_CTRL_CHECKABLE);
        }
    }
    lv_obj_add_event_cb(s_btnmatrix, btnmatrix_event_cb, LV_EVENT_ALL, NULL);

    /* ---- 触摸板 ---- */
    s_touchpad = lv_obj_create(scr);
    lv_obj_set_style_bg_color(s_touchpad, lv_color_hex(0x18222C), 0);
    lv_obj_set_style_border_color(s_touchpad, lv_color_hex(0x2E86C1), 0);
    lv_obj_set_style_border_width(s_touchpad, 1, 0);
    lv_obj_set_style_radius(s_touchpad, 6, 0);
    lv_obj_set_style_pad_all(s_touchpad, 4, 0);
    lv_obj_clear_flag(s_touchpad, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_touchpad, LV_OBJ_FLAG_EVENT_BUBBLE);

    s_tp_hint = lv_label_create(s_touchpad);
    lv_label_set_text(s_tp_hint, "触摸板\n拖动移动光标\n轻点=左键");
    lv_obj_set_style_text_color(s_tp_hint, lv_color_hex(0x5D6D7E), 0);
    lv_obj_set_style_text_align(s_tp_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_tp_hint);

    /* ---- 全屏触摸板模式下的左/右键 ---- */
    s_click_row = lv_obj_create(scr);
    lv_obj_set_style_bg_opa(s_click_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_click_row, 0, 0);
    lv_obj_set_style_pad_all(s_click_row, 3, 0);
    lv_obj_set_style_pad_column(s_click_row, 8, 0);
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

    /* ---- 底部模式栏 ---- */
    lv_obj_t *mode_bar = lv_obj_create(scr);
    lv_obj_set_size(mode_bar, SCREEN_W, MODE_BAR_H);
    lv_obj_set_pos(mode_bar, 0, SCREEN_H - MODE_BAR_H);
    lv_obj_set_style_bg_color(mode_bar, lv_color_hex(0x1E2A38), 0);
    lv_obj_set_style_border_width(mode_bar, 0, 0);
    lv_obj_set_style_radius(mode_bar, 0, 0);
    lv_obj_set_style_pad_all(mode_bar, 3, 0);
    lv_obj_clear_flag(mode_bar, LV_OBJ_FLAG_SCROLLABLE);

    static const char *mode_map[] = {"分屏", "全键盘", "全触摸板", ""};
    s_mode_btnm = lv_btnmatrix_create(mode_bar);
    lv_btnmatrix_set_map(s_mode_btnm, mode_map);
    lv_obj_set_size(s_mode_btnm, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_text_font(s_mode_btnm, &lv_font_kb_16, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_mode_btnm, lv_color_hex(0x34495E), LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_mode_btnm, lv_color_hex(0xECF0F1), LV_PART_ITEMS);
    lv_obj_set_style_radius(s_mode_btnm, 4, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_mode_btnm, lv_color_hex(0x2980B9),
                              LV_PART_ITEMS | LV_STATE_CHECKED);
    for (uint32_t i = 0; i < 3; i++) {
        lv_btnmatrix_set_btn_ctrl(s_mode_btnm, i, LV_BTNMATRIX_CTRL_CHECKABLE);
    }
    lv_obj_add_event_cb(s_mode_btnm, mode_bar_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    apply_mode(KB_MODE_SPLIT);

    ESP_LOGI(TAG, "keypad UI created (%u keys)", (unsigned)KB_KEY_COUNT);
}
