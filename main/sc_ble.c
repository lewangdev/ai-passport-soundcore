// main/sc_ble.c —— D3200 BLE 外设实现(NimBLE)。
#include "sc_ble.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "sc_ble";

// 广播名放在扫描响应里;客户端优先用服务 UUID 识别产品,名字只用于显示。
#define DEVICE_NAME         "soundcore Work"
#define D3200_SERVICE_UUID32 0x020CF5DAu
#define STOP_TIMEOUT_MS     2000

// 020cf5da-0000-1000-8000-00805f9b34fb(蓝牙基 UUID 派生,NimBLE 需小端字节序)
static const ble_uuid128_t SVC_UUID = BLE_UUID128_INIT(
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0xda, 0xf5, 0x0c, 0x02);
static const ble_uuid16_t WRITE_UUID = BLE_UUID16_INIT(0x7777);
static const ble_uuid16_t NOTIFY_UUID = BLE_UUID16_INIT(0x8888);
// 真机还暴露 OTA 服务(66666666-…/77777777-…)与 Tx Power 服务,照搬以保持服务表一致。
static const ble_uuid128_t OTA_SVC_UUID = BLE_UUID128_INIT(
    0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66);
static const ble_uuid128_t OTA_CHR_UUID = BLE_UUID128_INIT(
    0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77);
static const ble_uuid16_t TX_POWER_SVC_UUID = BLE_UUID16_INIT(0x1804);
static const ble_uuid16_t TX_POWER_LEVEL_UUID = BLE_UUID16_INIT(0x2A07);

static sc_ble_cbs_t s_cbs;
static sc_ble_adv_state_t s_adv;
static SemaphoreHandle_t s_tx_mutex;
static SemaphoreHandle_t s_host_stopped;
static TaskHandle_t s_host_task;
static bool s_initialized;
static volatile bool s_want_adv;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_subscribed;
static volatile uint16_t s_mtu = 23;
static uint16_t s_notify_handle;
static uint8_t s_addr_type;
static uint8_t s_mac[6];
static bool s_mac_valid;

static int gap_event(struct ble_gap_event *event, void *arg);

static int chr_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn;
    (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t buf[256];
        uint16_t len = 0;
        uint16_t total = OS_MBUF_PKTLEN(ctxt->om);
        if (total > sizeof(buf)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) != 0) return BLE_ATT_ERR_UNLIKELY;
        if (s_cbs.on_rx && len) s_cbs.on_rx(buf, len);
        return 0;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        ESP_LOGI(TAG, "读取特征 handle=%u", attr);
        // 通知特征也必须有 access_cb(否则整个服务注册失败);真机读出单字节 0x00。
        static const uint8_t zero = 0x00;
        return os_mbuf_append(ctxt->om, &zero, 1) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static int ota_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn;
    (void)attr;
    (void)arg;
    // 不支持 OTA:接受写入但丢弃(01 BB 已在命令层拒绝)。
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        ESP_LOGW(TAG, "忽略 OTA 特征写入 %u B", (unsigned)OS_MBUF_PKTLEN(ctxt->om));
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static int tx_power_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn;
    (void)attr;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    ESP_LOGI(TAG, "读取 Tx Power");
    static const int8_t level = 4;              // 与真机读数一致(dBm)
    return os_mbuf_append(ctxt->om, &level, 1) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static const struct ble_gatt_svc_def GATT_SVCS[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &TX_POWER_SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            { .uuid = &TX_POWER_LEVEL_UUID.u, .access_cb = tx_power_access, .flags = BLE_GATT_CHR_F_READ },
            { 0 },
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &OTA_SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &OTA_CHR_UUID.u,
                .access_cb = ota_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 },
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &WRITE_UUID.u,
                .access_cb = chr_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &NOTIFY_UUID.u,
                .access_cb = chr_access,
                .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ,
                .val_handle = &s_notify_handle,
            },
            { 0 },
        },
    },
    { 0 },
};

static void load_mac(void)
{
    if (s_mac_valid) return;
    uint8_t m[6];
    if (esp_read_mac(m, ESP_MAC_BT) == ESP_OK) {
        memcpy(s_mac, m, 6);
        s_mac_valid = true;
    }
}

void sc_ble_mac(uint8_t mac[6])
{
    load_mac();
    memcpy(mac, s_mac, 6);
}

static int advertise(void)
{
    // 主广播:flags + 32 位服务 UUID(0x020CF5DA = D3200)+ 厂商数据。
    // 厂商数据(含 company id 两字节)整体 8 字节,与真机抓包一致:
    //   [0..5] MAC  [6] flags(bit7 已绑定,0x70 颜色,0x0F 通道)  [7] 保留
    // 真机抓包(绑定在飞书):flags 0x91(颜色 1、通道 1),不带 "soundc" 标记。
    // 通道 1 时 soundcore App 搜不到设备,推测通道表示所属生态,这里用通道 0。
    uint8_t mfg[8];
    memcpy(mfg, s_mac, 6);
    mfg[6] = (uint8_t)((s_adv.bound ? 0x80 : 0x00) | 0x10);
    mfg[7] = 0x00;

    static ble_uuid32_t uuid32 = BLE_UUID32_INIT(D3200_SERVICE_UUID32);
    struct ble_hs_adv_fields fields = { 0 };
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids32 = &uuid32;
    fields.num_uuids32 = 1;
    fields.uuids32_is_complete = 1;
    fields.mfg_data = mfg;
    fields.mfg_data_len = sizeof(mfg);
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) return rc;

    struct ble_hs_adv_fields rsp = { 0 };
    rsp.name = (const uint8_t *)DEVICE_NAME;
    rsp.name_len = strlen(DEVICE_NAME);
    rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) return rc;

    struct ble_gap_adv_params params = { 0 };
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min = BLE_GAP_ADV_ITVL_MS(100);
    params.itvl_max = BLE_GAP_ADV_ITVL_MS(150);
    return ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
}

static void restart_adv(void)
{
    if (!s_want_adv || s_conn != BLE_HS_CONN_HANDLE_NONE) return;
    if (ble_gap_adv_active()) ble_gap_adv_stop();
    int rc = advertise();
    if (rc != 0) ESP_LOGE(TAG, "广播启动失败: %d", rc);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            s_mtu = 23;
            s_subscribed = false;
            struct ble_gap_conn_desc d;
            if (ble_gap_conn_find(s_conn, &d) == 0) {
                ESP_LOGI(TAG, "已连接 handle=%u 间隔=%u×1.25ms 延迟=%u 超时=%u×10ms", s_conn, d.conn_itvl,
                         d.conn_latency, d.supervision_timeout);
            }
            // 连接间隔 15~30 ms:实时音频需要约 9 KB/s 的通知吞吐。
            struct ble_gap_upd_params up = {
                .itvl_min = 12, .itvl_max = 24, .latency = 0, .supervision_timeout = 400,
                .min_ce_len = 0, .max_ce_len = 0,
            };
            ble_gap_update_params(s_conn, &up);
            int dl = ble_gap_set_data_len(s_conn, 251, 2120);
            if (dl != 0) ESP_LOGW(TAG, "请求数据长度扩展失败: %d", dl);
            // 2M PHY:每包空口时间减半,同样的连接间隔里能多发几包。
            ble_gap_set_prefered_le_phy(s_conn, BLE_GAP_LE_PHY_2M_MASK | BLE_GAP_LE_PHY_1M_MASK,
                                        BLE_GAP_LE_PHY_2M_MASK | BLE_GAP_LE_PHY_1M_MASK, BLE_GAP_LE_PHY_CODED_ANY);
            // MTU 交换由手机发起(iOS 自动、Android 客户端请求 247);本机只开 GATT 服务端。
            if (s_cbs.on_connect) s_cbs.on_connect();
        } else {
            restart_adv();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "已断开 reason=0x%x,历史最低空闲堆 %u B", event->disconnect.reason,
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_subscribed = false;
        s_mtu = 23;
        if (s_cbs.on_disconnect) s_cbs.on_disconnect();
        restart_adv();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "订阅变化 handle=%u notify=%u", event->subscribe.attr_handle, event->subscribe.cur_notify);
        if (event->subscribe.attr_handle == s_notify_handle) {
            s_subscribed = event->subscribe.cur_notify != 0;
            ESP_LOGI(TAG, "通知%s", s_subscribed ? "已订阅" : "已取消");
            if (s_cbs.on_subscribe) s_cbs.on_subscribe(s_subscribed);
        }
        break;
    case BLE_GAP_EVENT_CONN_UPDATE: {
        struct ble_gap_conn_desc d;
        if (ble_gap_conn_find(event->conn_update.conn_handle, &d) == 0) {
            ESP_LOGI(TAG, "连接参数 status=%d 间隔=%u×1.25ms 延迟=%u 超时=%u×10ms", event->conn_update.status,
                     d.conn_itvl, d.conn_latency, d.supervision_timeout);
        }
        break;
    }
    case BLE_GAP_EVENT_PHY_UPDATE_COMPLETE:
        ESP_LOGI(TAG, "PHY tx=%u rx=%u status=%d", event->phy_updated.tx_phy, event->phy_updated.rx_phy,
                 event->phy_updated.status);
        break;
    case BLE_GAP_EVENT_DATA_LEN_CHG:
        ESP_LOGI(TAG, "数据长度 tx=%u rx=%u", event->data_len_chg.max_tx_octets, event->data_len_chg.max_rx_octets);
        break;
    case BLE_GAP_EVENT_MTU:
        s_mtu = event->mtu.value;
        ESP_LOGI(TAG, "MTU=%u", s_mtu);
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        restart_adv();
        break;
    default:
        ESP_LOGI(TAG, "GAP 事件 %d", event->type);  // 排查官方 App:配对/加密等未处理事件
        break;
    }
    return 0;
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) rc = ble_hs_id_infer_auto(0, &s_addr_type);
    if (rc == 0) {
        uint8_t le[6];
        if (ble_hs_id_copy_addr(s_addr_type, le, NULL) == 0) {
            for (int i = 0; i < 6; i++) s_mac[i] = le[5 - i];
            s_mac_valid = true;
        }
        restart_adv();
    }
    if (rc != 0) ESP_LOGE(TAG, "NimBLE 同步失败: %d", rc);
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE 复位: %d", reason);
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    xSemaphoreGive(s_host_stopped);
    for (;;) vTaskSuspend(NULL);
}

esp_err_t sc_ble_start(const sc_ble_cbs_t *cbs, const sc_ble_adv_state_t *adv)
{
    if (s_initialized) return ESP_OK;
    s_cbs = *cbs;
    s_adv = *adv;
    load_mac();
    if (!s_tx_mutex) s_tx_mutex = xSemaphoreCreateMutex();
    if (!s_host_stopped) s_host_stopped = xSemaphoreCreateBinary();
    if (!s_tx_mutex || !s_host_stopped) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_host_stopped, 0);

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init 失败: %s", esp_err_to_name(err));
        return err;
    }
    s_initialized = true;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(GATT_SVCS);
    if (rc == 0) rc = ble_gatts_add_svcs(GATT_SVCS);
    if (rc == 0) rc = ble_svc_gap_device_name_set(DEVICE_NAME);
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT 注册失败: %d", rc);
        nimble_port_deinit();
        s_initialized = false;
        return ESP_FAIL;
    }
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    s_want_adv = true;
    if (xTaskCreatePinnedToCore(host_task, "nimble_host", NIMBLE_HS_STACK_SIZE, NULL,
                                configMAX_PRIORITIES - 4, &s_host_task, NIMBLE_CORE) != pdPASS) {
        s_want_adv = false;
        nimble_port_deinit();
        s_initialized = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "BLE 启动");
    return ESP_OK;
}

esp_err_t sc_ble_stop(void)
{
    if (!s_initialized) return ESP_OK;
    s_want_adv = false;
    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (ble_gap_adv_active()) ble_gap_adv_stop();
    int rc = nimble_port_stop();
    if (rc == 0 && xSemaphoreTake(s_host_stopped, pdMS_TO_TICKS(STOP_TIMEOUT_MS)) != pdTRUE) rc = -1;
    if (rc != 0) {
        ESP_LOGE(TAG, "NimBLE 停止失败: %d", rc);
        xSemaphoreGive(s_tx_mutex);
        return ESP_FAIL;
    }
    vTaskDelete(s_host_task);
    s_host_task = NULL;
    esp_err_t err = nimble_port_deinit();
    s_initialized = false;
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_subscribed = false;
    xSemaphoreGive(s_tx_mutex);
    ESP_LOGI(TAG, "BLE 已停止");
    return err;
}

bool sc_ble_running(void) { return s_initialized; }
bool sc_ble_connected(void) { return s_conn != BLE_HS_CONN_HANDLE_NONE; }
bool sc_ble_subscribed(void) { return s_conn != BLE_HS_CONN_HANDLE_NONE && s_subscribed; }
uint16_t sc_ble_mtu(void) { return s_mtu; }

void sc_ble_set_adv_state(const sc_ble_adv_state_t *adv)
{
    s_adv = *adv;
    if (s_initialized && ble_gap_adv_active()) restart_adv();
}

void sc_ble_disconnect(void)
{
    uint16_t c = s_conn;
    if (c != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(c, BLE_ERR_REM_USER_CONN_TERM);
}

#define SEND_HARD_LIMIT_MS 10000

esp_err_t sc_ble_send(const uint8_t *frame, size_t len, uint32_t timeout_ms)
{
    if (!s_initialized || !s_tx_mutex) return ESP_ERR_INVALID_STATE;
    TickType_t started = xTaskGetTickCount();
    TickType_t deadline = started + pdMS_TO_TICKS(timeout_ms);
    if (xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t err = ESP_OK;
    size_t off = 0;
    int last_rc = 0;
    while (off < len) {
        uint16_t conn = s_conn;
        if (conn == BLE_HS_CONN_HANDLE_NONE || !s_subscribed) {
            err = ESP_ERR_INVALID_STATE;
            break;
        }
        size_t chunk = (size_t)(s_mtu > 3 ? s_mtu - 3 : 20);
        if (chunk > len - off) chunk = len - off;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(frame + off, (uint16_t)chunk);
        int rc = om ? ble_gatts_notify_custom(conn, s_notify_handle, om) : BLE_HS_ENOMEM;
        if (rc == 0) {
            off += chunk;
            continue;
        }
        // 控制器/主机缓冲用尽:等待之前的通知发出再重试(om 已被消耗)。
        // 截止时间只约束第一包;帧已发出一部分时继续发完(或连接断开/硬上限),
        // 否则半帧会让客户端丢掉紧随其后的完整帧。
        last_rc = rc;
        TickType_t limit = off == 0 ? deadline : started + pdMS_TO_TICKS(SEND_HARD_LIMIT_MS);
        if ((rc == BLE_HS_ENOMEM || rc == BLE_HS_EBUSY) && (int32_t)(limit - xTaskGetTickCount()) > 0) {
            vTaskDelay(pdMS_TO_TICKS(4));
            continue;
        }
        err = rc == BLE_HS_ENOMEM && off == 0 ? ESP_ERR_TIMEOUT : ESP_FAIL;
        break;
    }
    xSemaphoreGive(s_tx_mutex);
    uint32_t waited_ms = (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount() - started);
    if (err != ESP_OK || waited_ms > 500) {
        ESP_LOGW(TAG, "通知发送 %u B 用时 %lu ms: %s(最后 rc=%d)", (unsigned)len, (unsigned long)waited_ms,
                 esp_err_to_name(err), last_rc);
    }
    return err;
}
