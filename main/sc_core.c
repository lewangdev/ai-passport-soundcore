// main/sc_core.c —— D3200 模拟器核心。
//
// 任务划分:
//   sc_core  —— 事件队列:BLE 收到的命令、连接事件、按键、Wi-Fi 事件、250 ms 节拍。
//               所有状态修改都在这里发生(录音开始/暂停/停止、绑定、握手、Wi-Fi 切换)。
//   sc_xfer  —— BLE 文件传输:按当前作业从存储读帧、加密、分片通知;实时作业跟随录音。
//   sc_audio —— 采集编码,帧回调直接写存储(sc_store 内部加锁)。
//   httpd    —— Wi-Fi WebSocket 命令在 HTTP 任务内同步处理(传输期间 BLE 已关闭)。
#include "sc_core.h"

#include "bsp_battery.h"
#include "sc_audio.h"
#include "sc_ble.h"
#include "sc_crypto.h"
#include "sc_store.h"
#include "sc_wifi.h"
#include "sc_xfer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/message_buffer.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "sc_core";

#define NVS_NS              "sc_dev"
#define FIRMWARE_VERSION    "01.00"
#define EV_DATA_MAX         256
#define TICK_MS             250
#define BATTERY_POLL_MS     10000
#define WIFI_SWITCH_WAIT_MS 3000    // 1A05 应答后等手机断开 BLE 的最长时间
#define WIFI_JOIN_TIMEOUT_S 240     // 热点开启后无人连接即关闭
#define WIFI_IDLE_TIMEOUT_S 90      // 会话无活动即关闭
#define WIFI_FINISH_DELAY_MS 1500
#define FIRST_FILE_ID       1735689600u  // 2025-01-01:未同步时钟时 id 从这里递增
#define XFER_BLE_UNITS      6       // BLE 每帧单元数(约 1 KB,按 MTU 拆成多个通知)
#define XFER_WS_UNITS       20      // Wi-Fi 每个 WebSocket 消息的单元数(约 3.3 KB)
// 编码帧先进消息缓冲,由写入任务落盘:LittleFS 擦块/同步可能阻塞几十毫秒,
// 不能卡住 I2S 采集。4 KB 约可缓冲 1.5 秒(16 kbps)。
#define FRAME_BUF_BYTES     4096
#define BIND_CONFIRM_MS     60000   // 绑定请求后等待用户按 OK 确认的时间

typedef enum {
    EV_BLE_RX,
    EV_BLE_CONNECT,
    EV_BLE_DISCONNECT,
    EV_BLE_SUBSCRIBE,
    EV_KEY,
    EV_WIFI,
    EV_STORAGE_FULL,
} ev_type_t;

typedef struct {
    ev_type_t type;
    uint16_t len;
    uint8_t a, b;
    uint8_t data[EV_DATA_MAX];
} ev_t;

typedef struct {
    uint32_t gen;
    bool active;
    uint32_t file_id;
    uint32_t start_seq;
    bool realtime;
} job_t;

// ---- 状态(s_mutex 保护跨任务读取的部分)----
static SemaphoreHandle_t s_mutex;
static QueueHandle_t s_queue;
static TaskHandle_t s_core_task;
static TaskHandle_t s_xfer_task;
static TaskHandle_t s_writer_task;
static MessageBufferHandle_t s_frame_buf;
static volatile bool s_writer_busy;
static volatile uint32_t s_frames_dropped;
static sc_rx_t s_rx;

static bool s_bound;
static bool s_find_my;
static uint32_t s_last_id;
static bool s_time_synced;
static sc_session_t s_session;
static bool s_keep_session;         // 1A05 之后会话要跨越 BLE 断开留给 Wi-Fi 使用
static uint8_t s_rec_status = SC_REC_STOPPED;
static uint32_t s_rec_file;
static bool s_connected;
static bool s_subscribed;
static sc_link_t s_link = SC_LINK_BLE_OFF;
static int s_battery_soc = -1;
static uint8_t s_battery_bucket = 0xFF;
static uint8_t s_mac[6];
static char s_serial[17];
static char s_toast[40];
static int64_t s_toast_until;

static job_t s_job;
static volatile bool s_xfer_busy;
static volatile bool s_xfer_realtime;
static volatile uint32_t s_xfer_file, s_xfer_sent, s_xfer_total;

static sc_wifi_creds_t s_wifi_creds;
static int64_t s_wifi_since;
static int64_t s_wifi_activity;
static int64_t s_wifi_finish_at;
static bool s_wifi_had_session;
static volatile uint32_t s_wifi_files;
static int64_t s_reset_at;
static volatile uint32_t s_total_kb, s_free_kb;    // 缓存:LittleFS 统计用量需要遍历文件系统

static void lock(void) { xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGiveRecursive(s_mutex); }
static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void refresh_usage(void)
{
    uint32_t total, free_kb;
    sc_store_usage(&total, &free_kb);
    s_total_kb = total;
    s_free_kb = free_kb;
}

static void toast_for(const char *msg, int64_t ms)
{
    lock();
    strlcpy(s_toast, msg, sizeof(s_toast));
    s_toast_until = now_ms() + ms;
    unlock();
}

static void toast(const char *msg)
{
    toast_for(msg, 2500);
}

// ---------------------------------------------------------------- 持久化

static void nvs_save_u32(const char *key, uint32_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, key, v);
    nvs_commit(h);
    nvs_close(h);
}

static void load_settings(void)
{
    nvs_handle_t h;
    uint32_t v;
    s_last_id = FIRST_FILE_ID;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    if (nvs_get_u32(h, "bound", &v) == ESP_OK) s_bound = v != 0;
    if (nvs_get_u32(h, "findmy", &v) == ESP_OK) s_find_my = v != 0;
    if (nvs_get_u32(h, "last_id", &v) == ESP_OK && v > s_last_id) s_last_id = v;
    nvs_close(h);
}

static uint32_t next_file_id(void)
{
    time_t now = time(NULL);
    uint32_t id = (s_time_synced && now > 0) ? (uint32_t)now : 0;
    if (id <= s_last_id) id = s_last_id + 1;
    s_last_id = id;
    nvs_save_u32("last_id", id);
    return id;
}

static uint32_t now_s(void)
{
    time_t now = time(NULL);
    return s_time_synced && now > 0 ? (uint32_t)now : s_last_id;
}

// ---------------------------------------------------------------- 发送

static void send_frame(uint8_t status, uint8_t type, uint8_t id, const uint8_t *payload, size_t len)
{
    uint8_t buf[160];
    size_t n = sc_frame_build(buf, sizeof(buf), status, type, id, payload, len);
    if (n == 0) {
        ESP_LOGE(TAG, "应答过长 %02X/%02X len=%u", type, id, (unsigned)len);
        return;
    }
    if (!sc_ble_subscribed()) return;
    esp_err_t err = sc_ble_send(buf, n, 1000);
    if (err != ESP_OK) ESP_LOGW(TAG, "发送 %02X/%02X 失败: %s", type, id, esp_err_to_name(err));
}

static void send_record_status(void)
{
    uint8_t p[5] = { s_rec_status };
    size_t n = 1;
    if (s_rec_status == SC_REC_STOPPED) {
        // 停止时附带时长(秒),对应 Feishu onAudioStatusChanged。
        sc_store_info_t info;
        uint32_t dur = (s_rec_file && sc_store_get(s_rec_file, &info)) ? info.frames / 50u : 0;
        sc_wr32(p + 1, dur);
        n = 5;
    }
    send_frame(1, SC_T_AUDIO, SC_ID_RECORD, p, n);
    if (s_rec_status == SC_REC_RECORDING && s_rec_file) {
        uint8_t q[5];
        sc_wr32(q, s_rec_file);
        q[4] = 1;
        send_frame(1, SC_T_FILE, SC_ID_RECORD_FILE, q, sizeof(q));
    }
}

static void build_devinfo(sc_devinfo_t *info)
{
    memset(info, 0, sizeof(*info));
    info->battery_bucket = sc_battery_bucket(s_battery_soc);
    info->box_battery_bucket = info->battery_bucket;
    strlcpy(info->firmware, FIRMWARE_VERSION, sizeof(info->firmware));
    strlcpy(info->box_firmware, FIRMWARE_VERSION, sizeof(info->box_firmware));
    strlcpy(info->wifi_firmware, FIRMWARE_VERSION, sizeof(info->wifi_firmware));
    strlcpy(info->serial, s_serial, sizeof(info->serial));
    info->total_kb = s_total_kb;
    info->free_kb = s_free_kb;
    // 没有充电盒:用本机 MAC 末字节翻转作盒子 MAC(客户端拒绝全相同的填充值)。
    memcpy(info->box_mac, s_mac, 6);
    info->box_mac[5] ^= 0x01;
    info->record_status = s_rec_status;
}

static size_t build_file_list(uint8_t *out, size_t cap, uint16_t page, bool with_end)
{
    static sc_file_entry_t entries[SC_STORE_MAX_FILES];
    size_t count = sc_store_list(entries, SC_STORE_MAX_FILES);
    uint32_t cur = s_rec_status != SC_REC_STOPPED ? s_rec_file : 0;
    int32_t dur = cur ? (int32_t)(sc_store_live_frames() / 50u) : 0;
    return sc_build_file_list(out, cap, entries, count, page, with_end, cur, dur);
}

// ---------------------------------------------------------------- BLE 传输作业

static void job_set(uint32_t file_id, uint32_t start_seq, bool realtime)
{
    lock();
    s_job.gen++;
    s_job.active = true;
    s_job.file_id = file_id;
    s_job.start_seq = start_seq;
    s_job.realtime = realtime;
    unlock();
    xTaskNotifyGive(s_xfer_task);
}

static void job_cancel(void)
{
    lock();
    s_job.gen++;
    s_job.active = false;
    unlock();
    xTaskNotifyGive(s_xfer_task);
}

static bool job_current(uint32_t gen)
{
    lock();
    bool same = s_job.gen == gen && s_job.active;
    unlock();
    return same;
}

static uint8_t s_xfer_bufs[XFER_BLE_UNITS][SC_UNIT_DATA];
static uint8_t s_xfer_lens[XFER_BLE_UNITS];
static uint8_t s_xfer_frame[SC_FRAME_OVERHEAD + XFER_BLE_UNITS * SC_UNIT_SIZE];

static void xfer_run(const job_t *job)
{
    sc_store_info_t info;
    sc_xfer_file_t file = { .file_id = job->file_id };
    if (!sc_store_get(job->file_id, &info)) {
        ESP_LOGW(TAG, "请求的文件不存在: %lu", (unsigned long)job->file_id);
        size_t n = sc_xfer_head_frame(s_xfer_frame, sizeof(s_xfer_frame), NULL, &file, 1);
        sc_ble_send(s_xfer_frame, n, 2000);
        n = sc_xfer_done_frame(s_xfer_frame, sizeof(s_xfer_frame), job->file_id);
        sc_ble_send(s_xfer_frame, n, 2000);
        return;
    }
    sc_session_t session;
    lock();
    session = s_session;
    unlock();
    file.size = info.frames * SC_UNIT_DATA;
    memcpy(file.file_key, info.file_key, 32);
    memcpy(file.nonce, info.nonce, 16);
    size_t n = sc_xfer_head_frame(s_xfer_frame, sizeof(s_xfer_frame), &session, &file, 0);
    if (sc_ble_send(s_xfer_frame, n, 2000) != ESP_OK) return;

    sc_reader_t *rd = sc_store_reader_open(job->file_id);
    if (!rd) return;
    uint32_t seq = job->start_seq;
    uint32_t end = info.frames;             // 离线传输:请求时的快照
    s_xfer_file = job->file_id;
    s_xfer_sent = seq;
    s_xfer_total = end;
    s_xfer_realtime = job->realtime;
    s_xfer_busy = true;
    ESP_LOGI(TAG, "BLE %s传输 %lu 从帧 %lu(共 %lu,%s)", job->realtime ? "实时" : "离线",
             (unsigned long)job->file_id, (unsigned long)seq, (unsigned long)end,
             file.encrypt ? "加密" : "明文");
    bool done = false;
    while (job_current(job->gen) && sc_ble_subscribed()) {
        int max = XFER_BLE_UNITS;
        if (!job->realtime && end - seq < (uint32_t)max) max = (int)(end - seq);
        int got = max > 0 ? sc_store_reader_read(rd, seq, s_xfer_bufs, s_xfer_lens, max) : 0;
        if (got < 0) {
            ESP_LOGW(TAG, "读取录音失败 %lu@%lu", (unsigned long)job->file_id, (unsigned long)seq);
            done = true;
            break;
        }
        if (got > 0) {
            n = sc_xfer_slice_frame(s_xfer_frame, sizeof(s_xfer_frame), &file, seq,
                                    (const uint8_t (*)[SC_UNIT_DATA])s_xfer_bufs, s_xfer_lens, (size_t)got);
            esp_err_t err = sc_ble_send(s_xfer_frame, n, 3000);
            // 链路暂时拥塞(一包都没发出):保持任务,重发同一批单元。
            if (err == ESP_ERR_TIMEOUT) continue;
            if (err != ESP_OK) break;
            seq += (uint32_t)got;
            s_xfer_sent = seq;
            continue;
        }
        if (!job->realtime) {
            done = true;
            break;
        }
        // 实时:已追上。录音结束(文件不再是正在录制的那条)且读完即完成。
        if (sc_store_live_id() != job->file_id) {
            sc_store_info_t now;
            if (!sc_store_get(job->file_id, &now) || seq >= now.frames) {
                done = true;
                break;
            }
        } else {
            s_xfer_total = sc_store_live_frames();
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
    }
    sc_store_reader_close(rd);
    if (done) {
        n = sc_xfer_done_frame(s_xfer_frame, sizeof(s_xfer_frame), job->file_id);
        sc_ble_send(s_xfer_frame, n, 2000);
        ESP_LOGI(TAG, "BLE 传输完成 %lu(%lu 帧)", (unsigned long)job->file_id, (unsigned long)seq);
    }
    s_xfer_busy = false;
}

static void xfer_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        job_t job;
        lock();
        job = s_job;
        unlock();
        if (!job.active) continue;
        xfer_run(&job);
        lock();
        if (s_job.gen == job.gen) s_job.active = false;
        bool again = s_job.active;
        unlock();
        if (again) xTaskNotifyGive(s_xfer_task);
    }
}

// ---------------------------------------------------------------- 录音控制

static void on_audio_frame(const uint8_t *frame, uint8_t len, void *ctx)
{
    (void)ctx;
    if (xMessageBufferSend(s_frame_buf, frame, len, 0) != len) s_frames_dropped++;
}

static void writer_task(void *arg)
{
    (void)arg;
    static uint8_t frame[SC_UNIT_DATA];
    for (;;) {
        size_t len = xMessageBufferReceive(s_frame_buf, frame, sizeof(frame), portMAX_DELAY);
        if (len == 0) continue;
        s_writer_busy = true;
        if (sc_store_append(frame, (uint8_t)len) != ESP_OK) {
            ev_t *ev = calloc(1, sizeof(ev_t));
            if (ev) {
                ev->type = EV_STORAGE_FULL;
                xQueueSend(s_queue, ev, 0);
                free(ev);
            }
        } else if (s_xfer_busy && s_xfer_realtime) {
            xTaskNotifyGive(s_xfer_task);       // 实时传输任务在等新帧
        }
        s_writer_busy = false;
    }
}

// 采集停止后,等写入任务把缓冲里的帧全部落盘。
static void writer_drain(void)
{
    for (int i = 0; i < 200 && (!xMessageBufferIsEmpty(s_frame_buf) || s_writer_busy); i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_frames_dropped) {
        ESP_LOGW(TAG, "写入跟不上,丢弃 %lu 帧", (unsigned long)s_frames_dropped);
        s_frames_dropped = 0;
    }
}

static bool record_start(void)
{
    if (s_link >= SC_LINK_WIFI_PENDING) {
        toast("Busy: Wi-Fi export");
        return false;
    }
    if (s_rec_status == SC_REC_RECORDING) return true;
    if (s_rec_status == SC_REC_STOPPED) {
        refresh_usage();
        if (s_free_kb < SC_STORE_MIN_FREE_KB) {
            toast("Storage full");
            return false;
        }
        uint32_t id = next_file_id();
        if (sc_store_begin(id, now_s()) != ESP_OK) {
            toast("Cannot create file");
            return false;
        }
        s_rec_file = sc_store_live_id();
    }
    if (sc_audio_start() != ESP_OK) {
        toast("Mic unavailable");
        return false;
    }
    lock();
    s_rec_status = SC_REC_RECORDING;
    unlock();
    ESP_LOGI(TAG, "录音中:文件 %lu", (unsigned long)s_rec_file);
    return true;
}

static void record_pause(void)
{
    if (s_rec_status != SC_REC_RECORDING) return;
    sc_audio_stop();
    writer_drain();
    sc_store_sync();
    lock();
    s_rec_status = SC_REC_PAUSED;
    unlock();
    ESP_LOGI(TAG, "录音暂停");
}

static void record_stop(void)
{
    if (s_rec_status == SC_REC_STOPPED) return;
    sc_audio_stop();
    writer_drain();
    sc_store_end(now_s());
    lock();
    s_rec_status = SC_REC_STOPPED;
    unlock();
    refresh_usage();
    // 实时传输任务读完剩余帧后发 1A0A。
    xTaskNotifyGive(s_xfer_task);
    ESP_LOGI(TAG, "录音停止:文件 %lu", (unsigned long)s_rec_file);
}

// ---------------------------------------------------------------- Wi-Fi

static void wifi_on_event(sc_wifi_event_t ev)
{
    ev_t e = { .type = EV_WIFI, .a = (uint8_t)ev };
    xQueueSend(s_queue, &e, 0);
}

static void ws_send_frame(sc_wifi_sink_t *sink, uint8_t type, uint8_t id, const uint8_t *p, size_t n)
{
    uint8_t buf[160];
    size_t len = sc_frame_build(buf, sizeof(buf), 1, type, id, p, n);
    if (len) sink->send(sink, buf, len);
}

static void ws_stream_file(const sc_transfer_req_t *req, sc_wifi_sink_t *sink)
{
    uint8_t(*bufs)[SC_UNIT_DATA] = malloc(XFER_WS_UNITS * SC_UNIT_DATA);
    uint8_t *frame = malloc(sc_xfer_slice_frame_size(XFER_WS_UNITS));
    uint8_t lens[XFER_WS_UNITS];
    size_t cap = sc_xfer_slice_frame_size(XFER_WS_UNITS);
    if (!bufs || !frame) {
        free(bufs);
        free(frame);
        return;
    }
    sc_store_info_t info;
    sc_xfer_file_t file = { .file_id = req->file_id };
    if (!sc_store_get(req->file_id, &info)) {
        size_t n = sc_xfer_head_frame(frame, cap, NULL, &file, 1);
        sink->send(sink, frame, n);
        goto out;
    }
    sc_session_t session;
    lock();
    session = s_session;
    unlock();
    file.size = info.frames * SC_UNIT_DATA;
    memcpy(file.file_key, info.file_key, 32);
    memcpy(file.nonce, info.nonce, 16);
    size_t n = sc_xfer_head_frame(frame, cap, &session, &file, 0);
    if (sink->send(sink, frame, n) != ESP_OK) goto out;

    sc_reader_t *rd = sc_store_reader_open(req->file_id);
    if (!rd) goto out;
    uint32_t seq = req->transferred / SC_UNIT_DATA;
    ESP_LOGI(TAG, "Wi-Fi 传输 %lu 从帧 %lu(共 %lu,%s)", (unsigned long)req->file_id,
             (unsigned long)seq, (unsigned long)info.frames, file.encrypt ? "加密" : "明文");
    bool ok = true;
    while (seq < info.frames) {
        int max = (int)(info.frames - seq < XFER_WS_UNITS ? info.frames - seq : XFER_WS_UNITS);
        int got = sc_store_reader_read(rd, seq, bufs, lens, max);
        if (got <= 0) break;
        n = sc_xfer_slice_frame(frame, cap, &file, seq, (const uint8_t (*)[SC_UNIT_DATA])bufs, lens, (size_t)got);
        if (sink->send(sink, frame, n) != ESP_OK) {
            ok = false;
            break;
        }
        seq += (uint32_t)got;
        lock();
        s_wifi_activity = now_ms();
        unlock();
    }
    sc_store_reader_close(rd);
    if (ok) {
        n = sc_xfer_done_frame(frame, cap, req->file_id);
        sink->send(sink, frame, n);
        s_wifi_files++;
        ESP_LOGI(TAG, "Wi-Fi 传输完成 %lu", (unsigned long)req->file_id);
    }
out:
    free(bufs);
    free(frame);
}

static void wifi_on_command(const sc_cmd_t *cmd, sc_wifi_sink_t *sink)
{
    sc_transfer_req_t req;
    if (sc_parse_transfer_req(cmd, &req)) {
        ws_stream_file(&req, sink);
        return;
    }
    if ((cmd->type == SC_T_FILE || cmd->type == SC_T_FILE_ALT) && cmd->id == SC_ID_FILE_LIST) {
        uint8_t p[140];
        uint16_t page = cmd->payload_len >= 2 ? sc_rd16(cmd->payload) : 0;
        size_t n = build_file_list(p, sizeof(p), page, cmd->type == SC_T_FILE_ALT);
        ws_send_frame(sink, cmd->type, SC_ID_FILE_LIST, p, n);
        return;
    }
    ESP_LOGI(TAG, "Wi-Fi 忽略命令 %02X/%02X", cmd->type, cmd->id);
}

static void ble_start(void);

static void wifi_switch(void)
{
    ESP_LOGI(TAG, "切换到 Wi-Fi:停止 BLE,开启热点「%s」", s_wifi_creds.ssid);
    job_cancel();
    sc_ble_stop();
    static const sc_wifi_cbs_t cbs = { .on_command = wifi_on_command, .on_event = wifi_on_event };
    esp_err_t err = sc_wifi_start(s_wifi_creds.ssid, s_wifi_creds.password, &cbs);
    lock();
    s_wifi_since = now_ms();
    s_wifi_activity = s_wifi_since;
    s_wifi_finish_at = 0;
    s_wifi_had_session = false;
    s_wifi_files = 0;
    s_link = err == ESP_OK ? SC_LINK_WIFI_AP : SC_LINK_BLE_OFF;
    unlock();
    if (err != ESP_OK) {
        toast("Wi-Fi failed");
        ble_start();
    }
}

static void wifi_end(const char *why)
{
    ESP_LOGI(TAG, "结束 Wi-Fi 会话:%s(导出 %lu 个文件)", why, (unsigned long)s_wifi_files);
    sc_wifi_stop();
    lock();
    s_link = SC_LINK_BLE_OFF;
    unlock();
    toast(why);
    ble_start();
}

// ---------------------------------------------------------------- 绑定

static int64_t s_bind_deadline;     // 0 = 没有待确认的绑定请求(仅 core 任务访问)
static volatile bool s_bind_pending; // 给输入任务看的镜像

static void set_bound(bool bound)
{
    lock();
    s_bound = bound;
    unlock();
    nvs_save_u32("bound", bound);
    sc_ble_adv_state_t adv = { .bound = bound };
    sc_ble_set_adv_state(&adv);
}

static void bind_confirm(void)
{
    s_bind_deadline = 0;
    s_bind_pending = false;
    set_bound(true);
    send_frame(1, SC_T_BIND, SC_ID_BIND_CONFIRM, NULL, 0);
    toast("Paired");
    ESP_LOGI(TAG, "绑定已确认");
}

static void bind_cancel(const char *why)
{
    if (!s_bind_deadline) return;
    s_bind_deadline = 0;
    s_bind_pending = false;
    toast(why);
    ESP_LOGI(TAG, "绑定未确认:%s", why);
}

// ---------------------------------------------------------------- 命令处理

static void handle_cmd(const sc_cmd_t *cmd, void *ctx)
{
    (void)ctx;
    uint8_t p[160];
    size_t n;
    ESP_LOGI(TAG, "命令 %02X/%02X len=%u%s", cmd->type, cmd->id, (unsigned)cmd->payload_len,
             cmd->checksum_ok ? "" : " (校验和错误)");
    if (cmd->payload_len && cmd->type != SC_T_ENCRYPT) {
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, cmd->payload, cmd->payload_len > 64 ? 64 : cmd->payload_len, ESP_LOG_INFO);
    }

    switch (cmd->type) {
    case SC_T_DEVICE:
        if (cmd->id == SC_ID_DEVICE_INFO) {
            sc_devinfo_t info;
            build_devinfo(&info);
            n = sc_build_devinfo(p, sizeof(p), &info);
            send_frame(1, SC_T_DEVICE, SC_ID_DEVICE_INFO, p, n);
        } else if (cmd->id == SC_ID_SYNC_TIME && cmd->payload_len >= 4) {
            struct timeval tv = { .tv_sec = (time_t)sc_rd32(cmd->payload), .tv_usec = 0 };
            settimeofday(&tv, NULL);
            int tz = cmd->payload_len >= 5 ? (int8_t)cmd->payload[4] : 0;
            char tzs[16];
            // POSIX TZ 符号与 UTC 偏移相反:UTC+8 → "UTC-8"。
            snprintf(tzs, sizeof(tzs), "UTC%+d", -tz);
            setenv("TZ", tzs, 1);
            tzset();
            lock();
            s_time_synced = true;
            unlock();
            send_frame(1, SC_T_DEVICE, SC_ID_SYNC_TIME, NULL, 0);
        } else if (cmd->id == SC_ID_RESET) {
            send_frame(1, SC_T_DEVICE, SC_ID_RESET, NULL, 0);
            s_reset_at = now_ms() + 500;
            toast("Factory reset...");
        } else {
            send_frame(0, SC_T_DEVICE, cmd->id, NULL, 0);   // OTA 等不支持
        }
        break;

    case SC_T_AUDIO:
        if (cmd->id == SC_ID_RECORD && cmd->payload_len >= 1) {
            if (cmd->payload[0] == 1) {
                if (record_start()) toast("Recording (app)");
            } else if (cmd->payload[0] == 2) {
                record_pause();
            }
            send_record_status();
        }
        break;

    case SC_T_FILE:
    case SC_T_FILE_ALT:
        switch (cmd->id) {
        case SC_ID_FILE_LIST: {
            uint16_t page = cmd->payload_len >= 2 ? sc_rd16(cmd->payload) : 0;
            n = build_file_list(p, sizeof(p), page, cmd->type == SC_T_FILE_ALT);
            send_frame(1, cmd->type, SC_ID_FILE_LIST, p, n);
            break;
        }
        case SC_ID_FILE_HEAD: {
            sc_transfer_req_t req;
            if (sc_parse_transfer_req(cmd, &req)) {
                job_set(req.file_id, req.transferred / SC_UNIT_DATA, req.realtime);
            }
            break;
        }
        case SC_ID_FILE_DELETE:
            if (cmd->payload_len >= 4) {
                uint32_t id = sc_rd32(cmd->payload);
                bool ok = sc_store_delete(id) == ESP_OK;
                refresh_usage();
                send_frame(ok ? 1 : 0, SC_T_FILE, SC_ID_FILE_DELETE, cmd->payload, 4);
            }
            break;
        case SC_ID_BT_START:
        case SC_ID_BT_STOP:
            // 没有经典蓝牙 SPP:确认即可,文件走 BLE 通知。
            send_frame(1, SC_T_FILE, cmd->id, cmd->payload, cmd->payload_len ? 1 : 0);
            break;
        case SC_ID_WIFI_CONFIG: {
            sc_wifi_creds_t creds;
            // 协议里 App 只能开始/暂停,没有"停止":暂停中的录音在导出前自动保存,
            // 否则只能靠设备长按 OK 才能导出。正在录音时仍拒绝。
            if (s_rec_status == SC_REC_PAUSED && s_link == SC_LINK_CONNECTED) record_stop();
            bool ok = s_rec_status == SC_REC_STOPPED && s_link == SC_LINK_CONNECTED &&
                      sc_parse_wifi_creds(cmd, &creds);
            if (!ok) {
                send_frame(0, SC_T_FILE, SC_ID_WIFI_CONFIG, NULL, 0);
                toast(s_rec_status != SC_REC_STOPPED ? "Stop recording first" : "Bad Wi-Fi config");
                break;
            }
            uint8_t ip[4];
            sc_wifi_ip(ip);
            n = sc_build_wifi_endpoint(p, sizeof(p), ip, SC_WIFI_PORT);
            send_frame(1, SC_T_FILE, SC_ID_WIFI_CONFIG, p, n);
            lock();
            s_wifi_creds = creds;
            s_keep_session = true;
            s_link = SC_LINK_WIFI_PENDING;
            s_wifi_since = now_ms();
            unlock();
            break;
        }
        case SC_ID_WIFI_CLOSE:
            send_frame(1, SC_T_FILE, SC_ID_WIFI_CLOSE, NULL, 0);
            break;
        default:
            break;
        }
        break;

    case SC_T_BIND:
        if (cmd->id == SC_ID_BIND && cmd->payload_len >= 1) {
            bool bind = cmd->payload[0] == 1;
            send_frame(1, SC_T_BIND, SC_ID_BIND, cmd->payload, 1);
            if (bind) {
                // 与原机一致:应答 0x87 后等用户在设备上按键,再发 0x88 确认。
                s_bind_deadline = now_ms() + BIND_CONFIRM_MS;
                s_bind_pending = true;
                toast_for("Press OK to pair", BIND_CONFIRM_MS);
                ESP_LOGI(TAG, "绑定请求:等待按 OK 确认");
            } else {
                s_bind_deadline = 0;
                s_bind_pending = false;
                set_bound(false);
                toast("Unbound");
            }
        }
        break;

    case SC_T_SETTINGS:
        if (cmd->id == SC_ID_FINDMY && cmd->payload_len >= 1) {
            lock();
            s_find_my = cmd->payload[0] != 0;
            unlock();
            nvs_save_u32("findmy", s_find_my);
            send_frame(1, SC_T_SETTINGS, SC_ID_FINDMY, cmd->payload, 1);
        }
        break;

    case SC_T_ENCRYPT:
        if (cmd->id == SC_ID_ENCRYPT_KEY && cmd->payload_len >= SC_PUBKEY_LEN) {
            uint8_t reply[SC_PUBKEY_LEN + SC_KEY_LEN];
            uint8_t key[SC_KEY_LEN];
            int64_t t0 = esp_timer_get_time();
            int rc = sc_crypto_handshake(cmd->payload, reply, reply + SC_PUBKEY_LEN, key);
            if (rc == 0) {
                lock();
                memcpy(s_session.key, key, sizeof(key));
                s_session.active = true;
                unlock();
                send_frame(1, SC_T_ENCRYPT, SC_ID_ENCRYPT_KEY, reply, sizeof(reply));
                ESP_LOGI(TAG, "ECDH 会话已建立(%lld ms)", (esp_timer_get_time() - t0) / 1000);
            } else {
                ESP_LOGW(TAG, "ECDH 握手失败: -0x%04x", -rc);
                send_frame(0, SC_T_ENCRYPT, SC_ID_ENCRYPT_KEY, NULL, 0);
            }
            memset(key, 0, sizeof(key));
        }
        break;

    default:
        break;
    }
}

static void on_ble_cmd(const sc_cmd_t *cmd, void *ctx)
{
    handle_cmd(cmd, ctx);
}

// ---------------------------------------------------------------- BLE 回调(NimBLE 任务)

static void post(ev_type_t type, const uint8_t *data, size_t len, uint8_t a)
{
    ev_t ev = { .type = type, .a = a };
    if (len > sizeof(ev.data)) len = sizeof(ev.data);
    if (len) memcpy(ev.data, data, len);
    ev.len = (uint16_t)len;
    if (xQueueSend(s_queue, &ev, pdMS_TO_TICKS(50)) != pdTRUE) ESP_LOGW(TAG, "事件队列满,丢弃 %d", type);
}

static void cb_connect(void) { post(EV_BLE_CONNECT, NULL, 0, 0); }
static void cb_disconnect(void) { post(EV_BLE_DISCONNECT, NULL, 0, 0); }
static void cb_subscribe(bool on) { post(EV_BLE_SUBSCRIBE, NULL, 0, on); }
static void cb_rx(const uint8_t *data, size_t len)
{
    while (len > 0) {
        size_t n = len > EV_DATA_MAX ? EV_DATA_MAX : len;
        post(EV_BLE_RX, data, n, 0);
        data += n;
        len -= n;
    }
}

static void ble_start(void)
{
    static const sc_ble_cbs_t cbs = {
        .on_connect = cb_connect,
        .on_disconnect = cb_disconnect,
        .on_subscribe = cb_subscribe,
        .on_rx = cb_rx,
    };
    sc_ble_adv_state_t adv = { .bound = s_bound };
    esp_err_t err = sc_ble_start(&cbs, &adv);
    lock();
    s_link = err == ESP_OK ? SC_LINK_ADVERTISING : SC_LINK_BLE_OFF;
    s_connected = false;
    s_subscribed = false;
    unlock();
    if (err != ESP_OK) ESP_LOGE(TAG, "BLE 启动失败: %s", esp_err_to_name(err));
}

// ---------------------------------------------------------------- 事件处理

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (btn != BSP_BTN_OK) return;              // 翻页键由界面处理
    if (s_bind_deadline && (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG)) {
        if (s_subscribed) bind_confirm();
        else bind_cancel("Pairing cancelled");
        return;
    }
    if (ev == BSP_BTN_CLICK) {
        if (s_link >= SC_LINK_WIFI_PENDING) return;
        if (s_rec_status == SC_REC_RECORDING) {
            record_pause();
            toast("Paused");
        } else if (record_start()) {
            toast("Recording");
        }
        send_record_status();
    } else if (ev == BSP_BTN_LONG) {
        if (s_link >= SC_LINK_WIFI_AP) {
            wifi_end("Wi-Fi cancelled");
        } else if (s_rec_status != SC_REC_STOPPED) {
            record_stop();
            send_record_status();
            toast("Saved");
        }
    }
}

static void tick(void)
{
    static int64_t last_battery;
    int64_t t = now_ms();

    if (t - last_battery >= BATTERY_POLL_MS || last_battery == 0) {
        last_battery = t;
        int soc = bsp_battery_soc();
        uint8_t bucket = sc_battery_bucket(soc);
        lock();
        s_battery_soc = soc;
        unlock();
        if (bucket != s_battery_bucket) {
            s_battery_bucket = bucket;
            uint8_t p[2] = { bucket, bucket };
            send_frame(1, SC_T_DEVICE, SC_ID_BATTERY, p, sizeof(p));
        }
    }

    if (s_bind_deadline && t > s_bind_deadline) bind_cancel("Pairing timed out");

    static int64_t last_space;
    if (t - last_space > 5000) {
        last_space = t;
        refresh_usage();
        if (s_rec_status == SC_REC_RECORDING) {
            if (s_free_kb < SC_STORE_MIN_FREE_KB / 2) {
                record_stop();
                send_record_status();
                toast("Storage full - saved");
            }
        }
    }

    if (s_link == SC_LINK_WIFI_PENDING &&
        (!s_connected || t - s_wifi_since > WIFI_SWITCH_WAIT_MS)) {
        wifi_switch();
    } else if (s_link == SC_LINK_WIFI_AP || s_link == SC_LINK_WIFI_CLIENT) {
        if (s_wifi_finish_at && t >= s_wifi_finish_at) {
            wifi_end("Wi-Fi export done");
        } else if (!s_wifi_had_session && t - s_wifi_since > WIFI_JOIN_TIMEOUT_S * 1000LL) {
            wifi_end("Wi-Fi timeout");
        } else if (s_wifi_had_session && t - s_wifi_activity > WIFI_IDLE_TIMEOUT_S * 1000LL) {
            wifi_end("Wi-Fi idle");
        }
    }

    if (s_reset_at && t >= s_reset_at) {
        ESP_LOGW(TAG, "恢复出厂设置");
        record_stop();
        sc_store_wipe();
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_all(h);
            nvs_commit(h);
            nvs_close(h);
        }
        esp_restart();
    }
}

static void handle_event(const ev_t *ev)
{
    switch (ev->type) {
    case EV_BLE_RX:
        sc_rx_feed(&s_rx, ev->data, ev->len, on_ble_cmd, NULL);
        break;
    case EV_BLE_CONNECT:
        sc_rx_init(&s_rx, true);
        lock();
        s_connected = true;
        // 新连接:旧会话只保留给紧随其后的 Wi-Fi 导出,本次连接重新握手。
        s_keep_session = false;
        unlock();
        break;
    case EV_BLE_DISCONNECT:
        job_cancel();
        bind_cancel("Pairing cancelled");
        lock();
        s_connected = false;
        s_subscribed = false;
        if (s_link == SC_LINK_CONNECTED) s_link = SC_LINK_ADVERTISING;
        if (!s_keep_session) {
            memset(&s_session, 0, sizeof(s_session));
        }
        unlock();
        break;
    case EV_BLE_SUBSCRIBE:
        lock();
        s_subscribed = ev->a != 0;
        if (s_link == SC_LINK_ADVERTISING && s_subscribed) s_link = SC_LINK_CONNECTED;
        unlock();
        if (s_subscribed) {
            toast("App connected");
            s_battery_bucket = 0xFF;            // 重新推送一次电量
        }
        break;
    case EV_KEY:
        on_key((bsp_btn_t)ev->a, (bsp_btn_ev_t)ev->b);
        break;
    case EV_STORAGE_FULL:
        if (s_rec_status != SC_REC_STOPPED) {
            record_stop();
            send_record_status();
            toast("Storage full - saved");
        }
        break;
    case EV_WIFI: {
        int64_t t = now_ms();
        lock();
        s_wifi_activity = t;
        switch ((sc_wifi_event_t)ev->a) {
        case SC_WIFI_EV_STA_JOINED:
        case SC_WIFI_EV_WS_OPEN:
            if (s_link == SC_LINK_WIFI_AP) s_link = SC_LINK_WIFI_CLIENT;
            if (ev->a == SC_WIFI_EV_WS_OPEN) s_wifi_had_session = true;
            break;
        case SC_WIFI_EV_FINISH:
            s_wifi_finish_at = t + WIFI_FINISH_DELAY_MS;
            break;
        case SC_WIFI_EV_STA_LEFT:
            // 手机离开热点且已完成过会话:尽快收尾。
            if (s_wifi_had_session && !s_wifi_finish_at) s_wifi_finish_at = t + WIFI_FINISH_DELAY_MS;
            break;
        default:
            break;
        }
        unlock();
        break;
    }
    }
}

static void core_task(void *arg)
{
    (void)arg;
    static ev_t ev;
    TickType_t last_tick = xTaskGetTickCount();
    for (;;) {
        if (xQueueReceive(s_queue, &ev, pdMS_TO_TICKS(TICK_MS)) == pdTRUE) handle_event(&ev);
        if (xTaskGetTickCount() - last_tick >= pdMS_TO_TICKS(TICK_MS)) {
            last_tick = xTaskGetTickCount();
            tick();
        }
    }
}

// ---------------------------------------------------------------- 对外接口

esp_err_t sc_core_start(void)
{
    s_mutex = xSemaphoreCreateRecursiveMutex();
    s_queue = xQueueCreate(8, sizeof(ev_t));
    s_frame_buf = xMessageBufferCreate(FRAME_BUF_BYTES);
    if (!s_mutex || !s_queue || !s_frame_buf) return ESP_ERR_NO_MEM;
    load_settings();
    sc_rx_init(&s_rx, true);
    refresh_usage();

    sc_ble_mac(s_mac);
    snprintf(s_serial, sizeof(s_serial), "apd3%02x%02x%02x%02x%02x%02x", s_mac[0], s_mac[1], s_mac[2],
             s_mac[3], s_mac[4], s_mac[5]);

    esp_err_t err = sc_audio_init(on_audio_frame, NULL);
    if (err != ESP_OK) ESP_LOGE(TAG, "音频初始化失败: %s(无法录音)", esp_err_to_name(err));

    if (xTaskCreate(xfer_task, "sc_xfer", 6144, NULL, 5, &s_xfer_task) != pdPASS) return ESP_ERR_NO_MEM;
    if (xTaskCreate(writer_task, "sc_writer", 4096, NULL, 6, &s_writer_task) != pdPASS) return ESP_ERR_NO_MEM;
    if (xTaskCreate(core_task, "sc_core", 6144, NULL, 5, &s_core_task) != pdPASS) return ESP_ERR_NO_MEM;
    ble_start();
    return ESP_OK;
}

bool sc_core_bind_pending(void)
{
    return s_bind_pending;
}

void sc_core_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (!s_queue) return;
    ev_t e = { .type = EV_KEY, .a = (uint8_t)btn, .b = (uint8_t)ev };
    xQueueSend(s_queue, &e, 0);
}

void sc_core_snapshot(sc_snapshot_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!s_mutex) return;
    lock();
    out->link = s_link;
    out->bound = s_bound;
    out->session = s_session.active;
    out->find_my = s_find_my;
    out->time_synced = s_time_synced;
    out->rec_status = s_rec_status;
    out->rec_file = s_rec_status != SC_REC_STOPPED ? s_rec_file : 0;
    out->battery_soc = s_battery_soc;
    strlcpy(out->ssid, s_wifi_creds.ssid, sizeof(out->ssid));
    strlcpy(out->password, s_wifi_creds.password, sizeof(out->password));
    strlcpy(out->serial, s_serial, sizeof(out->serial));
    memcpy(out->mac, s_mac, 6);
    if (now_ms() < s_toast_until) strlcpy(out->toast, s_toast, sizeof(out->toast));
    unlock();
    out->rec_seconds = out->rec_file ? sc_store_live_frames() / 50u : 0;
    out->level = sc_audio_level();
    out->streaming = s_xfer_busy;
    out->streaming_realtime = s_xfer_realtime;
    out->xfer_file = s_xfer_file;
    out->xfer_sent = s_xfer_sent;
    out->xfer_total = s_xfer_total;
    out->wifi_files = s_wifi_files;
    out->file_count = (uint32_t)sc_store_count();
    out->total_kb = s_total_kb;
    out->free_kb = s_free_kb;
}

size_t sc_core_recent_files(sc_file_entry_t *out, size_t cap)
{
    return sc_store_list(out, cap);
}
