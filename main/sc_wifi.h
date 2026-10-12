// main/sc_wifi.h —— Wi-Fi 快速导出:SoftAP + wss:// WebSocket 服务。
//
// 流程(与客户端 WifiExportService 对应):
//   BLE 1A05 下发 SSID/密码 → 设备应答 IP:端口 → 手机断开 BLE、加入热点 →
//   wss://192.168.4.1:443 → 文本帧发十六进制命令(1A07 请求文件)→ 设备回二进制帧
//   (1A07 头 / 1A08 分片 / 1A0A 完成)→ 文本 "FINISH" 结束会话。
// TLS 证书为设备首次使用时自生成的自签名 P-256 证书(客户端信任任意证书),缓存在 NVS。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sc_proto.h"

#define SC_WIFI_PORT 443

typedef struct sc_wifi_sink {
    esp_err_t (*send)(struct sc_wifi_sink *sink, const uint8_t *data, size_t len);
    void *ctx;
} sc_wifi_sink_t;

typedef enum {
    SC_WIFI_EV_STA_JOINED,
    SC_WIFI_EV_STA_LEFT,
    SC_WIFI_EV_WS_OPEN,
    SC_WIFI_EV_WS_CLOSED,
    SC_WIFI_EV_FINISH,          // 客户端发来 "FINISH" 或 1A0A
    SC_WIFI_EV_ACTIVITY,
} sc_wifi_event_t;

typedef struct {
    // HTTP 服务任务中调用;可同步地通过 sink 发送任意多帧(本函数返回前 sink 有效)。
    void (*on_command)(const sc_cmd_t *cmd, sc_wifi_sink_t *sink);
    void (*on_event)(sc_wifi_event_t ev);
} sc_wifi_cbs_t;

// 预先准备 TLS 证书(首次需生成,约 1 秒)。可在空闲时提前调用。
esp_err_t sc_wifi_prepare_cert(void);
esp_err_t sc_wifi_start(const char *ssid, const char *password, const sc_wifi_cbs_t *cbs);
void sc_wifi_stop(void);
bool sc_wifi_running(void);
void sc_wifi_ip(uint8_t ip[4]);
