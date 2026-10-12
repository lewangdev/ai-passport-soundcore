// main/sc_ble.h —— D3200 BLE 外设:广播、GATT(WRITE 0x7777 / NOTIFY 0x8888)、通知发送。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    void (*on_connect)(void);
    void (*on_disconnect)(void);
    void (*on_subscribe)(bool enabled);
    // NimBLE host 任务中调用:只拷贝/入队,不要做重活。
    void (*on_rx)(const uint8_t *data, size_t len);
} sc_ble_cbs_t;

// 广播里需要的设备身份。
typedef struct {
    bool bound;                 // 厂商数据 flags bit7
} sc_ble_adv_state_t;

esp_err_t sc_ble_start(const sc_ble_cbs_t *cbs, const sc_ble_adv_state_t *adv);
esp_err_t sc_ble_stop(void);
bool sc_ble_running(void);
bool sc_ble_connected(void);
bool sc_ble_subscribed(void);
uint16_t sc_ble_mtu(void);
// 更新广播内容(绑定位变化后调用);未连接时立即生效。
void sc_ble_set_adv_state(const sc_ble_adv_state_t *adv);
void sc_ble_disconnect(void);
// 本机 BLE 地址(显示顺序 AA:BB:...),start 前也可调用。
void sc_ble_mac(uint8_t mac[6]);

// 发送一条完整协议帧:按 MTU 拆成多个通知,发送期间持锁,保证帧之间不交错。
// 通知缓冲不足时退避重试:第一包在 timeout_ms 内发不出返回 ESP_ERR_TIMEOUT(什么都没发,
// 可整帧重发);已开始发送的帧会发完,除非连接断开或超过 10 s 硬上限(返回 ESP_FAIL)。
esp_err_t sc_ble_send(const uint8_t *frame, size_t len, uint32_t timeout_ms);
