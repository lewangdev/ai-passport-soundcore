// main/sc_ui.h —— 模拟器界面:录音页 / 录音列表 / 设备信息,Wi-Fi 导出时自动切到热点页。
//
// 所有 LVGL 对象只在 LVGL 任务中(lv_timer 回调)访问;外部只改原子页码。
#pragma once

#include <stdbool.h>

#include "esp_err.h"

// 须在 bsp_lvgl_init() 之后、持锁调用。
esp_err_t sc_ui_init(void);
// UP/DOWN 翻页(任意任务可调用)。
void sc_ui_page_step(int delta);
// Wi-Fi 导出页是否正在显示(用于背光策略)。
bool sc_ui_wifi_page(void);
