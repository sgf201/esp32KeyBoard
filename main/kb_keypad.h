/**
 * @file kb_keypad.h
 *
 * 虚拟键盘 + 触摸板界面，支持多指同时按键。
 *
 * 三种模式（底部栏切换）：
 *
 *   分屏      键盘在左 62%，触摸板在右 38%
 *   全键盘    键盘占满主区域
 *   全触摸板  触摸板占满，底部附左/右键
 *
 * 触摸处理不经过 LVGL 的 indev（LVGL v8 是单点模型，无法表达两个手指同时
 * 按下两个键），而是由独立任务读取 GT911 多点快照自己做命中测试。
 */
#ifndef KB_KEYPAD_H
#define KB_KEYPAD_H

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KB_MODE_SPLIT = 0,   /* 左键盘 + 右触摸板 */
    KB_MODE_KEYBOARD,    /* 全屏键盘 */
    KB_MODE_TOUCHPAD,    /* 全屏触摸板 */
} kb_mode_t;

/**
  * @brief  构建界面、创建触摸任务并切到分屏模式。
  *
  * 必须在 LVGL 初始化、显示驱动注册之后调用。
  */
void kb_ui_create(void);

/**
  * @brief  注入保护 LVGL 的互斥量（可选）。
  *
  * 本模块的视觉刷新走 LVGL 定时器，本身已在 LVGL 上下文执行；这个接口留给
  * 将来需要在其它任务里操作界面的场景。
  */
void kb_ui_set_gui_lock(SemaphoreHandle_t lock);

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
