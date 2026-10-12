// main/sc_core.h —— D3200 模拟器核心:命令分发、录音状态机、BLE/Wi-Fi 传输调度。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "bsp_button.h"
#include "esp_err.h"
#include "sc_proto.h"

typedef enum {
    SC_LINK_BLE_OFF = 0,        // BLE 未运行(启动中 / 切换到 Wi-Fi 中)
    SC_LINK_ADVERTISING,
    SC_LINK_CONNECTED,          // 已连接且订阅通知
    SC_LINK_WIFI_PENDING,       // 已应答 1A05,等手机断开 BLE 后开热点
    SC_LINK_WIFI_AP,            // 热点已开,等手机加入
    SC_LINK_WIFI_CLIENT,        // 手机已加入热点 / WebSocket 已连接
} sc_link_t;

typedef struct {
    sc_link_t link;
    bool bound;
    bool session;               // ECDH 会话已建立(文件加密传输)
    bool find_my;
    bool time_synced;
    uint8_t rec_status;         // SC_REC_*
    uint32_t rec_file;
    uint32_t rec_seconds;
    uint8_t level;              // 麦克风电平 0..100
    bool streaming;             // BLE 实时/离线传输进行中
    bool streaming_realtime;
    uint32_t xfer_file;
    uint32_t xfer_sent;         // 已发送帧
    uint32_t xfer_total;        // 帧(实时传输为当前帧数)
    uint32_t wifi_files;        // 本次 Wi-Fi 会话已导出文件数
    uint32_t file_count;
    uint32_t total_kb;
    uint32_t free_kb;
    int battery_soc;            // -1 = 不可用
    char ssid[33];
    char password[65];
    char serial[17];
    uint8_t mac[6];
    char toast[40];             // 最近一条状态提示(空 = 无)
} sc_snapshot_t;

esp_err_t sc_core_start(void);
// 输入任务调用(非按键回调上下文)。
void sc_core_key(bsp_btn_t btn, bsp_btn_ev_t ev);
// App 发起绑定、等待用户按 OK 确认中(界面需保持点亮)。
bool sc_core_bind_pending(void);
void sc_core_snapshot(sc_snapshot_t *out);
// 最近的录音清单(新的在前),界面用。返回条数。
size_t sc_core_recent_files(sc_file_entry_t *out, size_t cap);
