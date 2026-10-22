/**
 * @file kb_keypad.h
 *
 * 虚拟键盘 + 触摸板界面。
 *
 * 布局（800x480 横屏）：
 *
 *   +--------------------------------------------------+
 *   |  状态条：连接状态 / 电量 / 当前修饰键              |  40px
 *   +--------------------------------------------------+
 *   |                                                  |
 *   |  主区域（按模式变化）                             |
 *   |    SPLIT      上半键盘 / 下半触摸板               |
 *   |    KEYBOARD   全屏键盘                            |
 *   |    TOUCHPAD   全屏触摸板                          |
 *   |                                                  |
 *   +--------------------------------------------------+
 *   |  模式切换：分屏 | 全键盘 | 全触摸板                |  44px
 *   +--------------------------------------------------+
 */
#ifndef KB_KEYPAD_H
#define KB_KEYPAD_H

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KB_MODE_SPLIT = 0,   /* keyboard on top, touchpad below */
    KB_MODE_KEYBOARD,    /* full-screen keyboard */
    KB_MODE_TOUCHPAD,    /* full-screen touchpad */
} kb_mode_t;

/**
  * @brief  构建整个界面并切换到默认模式（分屏）。
  *
  * 必须在 LVGL 初始化、显示驱动注册完成之后调用。
  */
void kb_ui_create(void);

/**
  * @brief  切换模式。
  */
void kb_ui_set_mode(kb_mode_t mode);

/**
  * @brief  当前模式。
  */
kb_mode_t kb_ui_get_mode(void);

#ifdef __cplusplus
}
#endif

#endif /* KB_KEYPAD_H */
