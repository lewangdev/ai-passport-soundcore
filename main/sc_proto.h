// main/sc_proto.h —— soundcore Work (D3200) 线协议:帧收发、命令解析与应答载荷构造。
//
// 纯逻辑,不依赖 ESP-IDF / LVGL,可在 host 上测试(tests/test_sc_proto.c)。
// 协议来源:tacshi/Soundcore 的 PROTOCOL.md 与其 Flutter 客户端实现。
//
// 手机 → 设备(命令):
//   08 EE 00 00 00 | type | id | len u16 LE(整帧长度) | payload | checksum
// 设备 → 手机(应答/推送):
//   09 FF 00 00 | status | type | id | len u16 LE(整帧长度) | payload | checksum
// checksum = 之前所有字节之和的低 8 位。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SC_HDR_LEN          9u      // magic(5/4+status) + type + id + len
#define SC_FRAME_OVERHEAD   10u     // header + checksum
#define SC_UNIT_DATA        160u    // 每个音频单元固定 160 字节(Opus 帧,尾部补零)
#define SC_UNIT_SIZE        166u    // seq u32 | flags u8 | data 160 | pad u8
#define SC_RX_BUF_SIZE      512u    // 命令重组缓冲;最长命令是 1A05(SSID+密码)
#define SC_FILE_PAGE_SIZE   10u     // 1A0E/1B0E 每页条目数(客户端以"满 10 条"判断翻页)
#define SC_DEVINFO_LEN      57u     // 设备信息载荷长度(含 1 字节保留位,见 .c)
#define SC_FILE_HEAD_LEN    87u     // 加密文件头载荷:id+size+nonce+key+sessNonce+err
#define SC_FILE_KEY_MAGIC   "soundcored3200"
#define SC_FILE_KEY_MAGIC_LEN 14u
#define SC_WRAPPED_KEY_LEN  46u     // magic(14) + fileKey(32)

// ---- 命令字(type, id)----
enum {
    SC_T_DEVICE   = 0x01,
    SC_T_BIND     = 0x0B,
    SC_T_SETTINGS = 0x10,
    SC_T_AUDIO    = 0x18,
    SC_T_FILE     = 0x1A,
    SC_T_FILE_ALT = 0x1B,
    SC_T_ENCRYPT  = 0x2E,
    SC_T_LOG      = 0xFF,
};

enum {
    SC_ID_DEVICE_INFO   = 0x01,
    SC_ID_BATTERY       = 0x03,
    SC_ID_CHARGING      = 0x04,
    SC_ID_SYNC_TIME     = 0xA6,
    SC_ID_RESET         = 0xB8,
    SC_ID_OTA_MODE      = 0xBB,

    SC_ID_BIND          = 0x87,
    SC_ID_BIND_CONFIRM  = 0x88,

    SC_ID_FINDMY        = 0xA2,

    SC_ID_RECORD        = 0x82,

    SC_ID_WIFI_CLOSE    = 0x02,
    SC_ID_WIFI_CONFIG   = 0x05,
    SC_ID_RECORD_FILE   = 0x06,
    SC_ID_FILE_HEAD     = 0x07,
    SC_ID_FILE_SLICE    = 0x08,
    SC_ID_FILE_DONE     = 0x0A,
    SC_ID_FILE_LIST     = 0x0E,
    SC_ID_BT_START      = 0x0F,
    SC_ID_FILE_DELETE   = 0x10,
    SC_ID_BT_STOP       = 0x11,

    SC_ID_ENCRYPT_KEY   = 0x01,
};

// 录音线状态(0x18/0x82 载荷与设备信息 recording 字节)
enum {
    SC_REC_STOPPED = 0,
    SC_REC_RECORDING = 1,
    SC_REC_PAUSED = 2,
};

// ---- 校验 ----
uint8_t sc_checksum(const uint8_t *data, size_t len);

// ---- 命令重组(手机 → 设备)----
typedef struct {
    uint8_t type;
    uint8_t id;
    const uint8_t *payload;     // 指向重组缓冲内部,仅在回调期间有效
    size_t payload_len;
    bool checksum_ok;
} sc_cmd_t;

typedef void (*sc_cmd_cb_t)(const sc_cmd_t *cmd, void *ctx);

typedef struct {
    uint8_t buf[SC_RX_BUF_SIZE];
    size_t len;
    // false:校验和不符也交付(WebSocket 文本请求末字节是 0x1B,不是校验和)。
    bool require_checksum;
    uint32_t dropped_bytes;     // 统计:被丢弃的非帧字节(便于日志排查)
} sc_rx_t;

void sc_rx_init(sc_rx_t *rx, bool require_checksum);
// 喂入任意分片字节;每解出一条完整命令调用一次 cb。返回交付的命令条数。
size_t sc_rx_feed(sc_rx_t *rx, const uint8_t *data, size_t len, sc_cmd_cb_t cb, void *ctx);

// ---- 应答帧(设备 → 手机)----
// 写出完整帧,返回帧长;cap 不足或载荷过长返回 0。payload 可为 NULL(plen=0)。
size_t sc_frame_build(uint8_t *out, size_t cap, uint8_t status, uint8_t type, uint8_t id,
                      const uint8_t *payload, size_t plen);

// ---- 载荷构造 ----
typedef struct {
    uint8_t battery_bucket;     // 0..9,显示为 (bucket+1)*10%
    bool charging;
    char firmware[6];           // 5 字节 ASCII,如 "01.00"
    char serial[17];            // 16 字节 ASCII(小写)
    uint32_t total_kb;
    uint32_t free_kb;
    bool box_charging;
    char box_firmware[6];
    uint8_t box_battery_bucket;
    uint8_t box_mac[6];
    uint8_t color;
    uint8_t record_status;      // SC_REC_*
    char wifi_firmware[6];
} sc_devinfo_t;

// 写出 SC_DEVINFO_LEN 字节。
size_t sc_build_devinfo(uint8_t *out, size_t cap, const sc_devinfo_t *info);

typedef struct {
    uint32_t file_id;
    uint32_t end_time;
    uint32_t size;              // 字节,= 帧数 x 160
} sc_file_entry_t;

// 1A0E(with_end=false,每条 8 字节)/ 1B0E(with_end=true,每条 12 字节)单页。
// entries 为全部文件;page 从 0 开始,每页 SC_FILE_PAGE_SIZE 条。
size_t sc_build_file_list(uint8_t *out, size_t cap, const sc_file_entry_t *entries, size_t count,
                          uint16_t page, bool with_end, uint32_t current_id, int32_t current_dur);

// 文件头(1A07)。keys==NULL 时只写 fileId+size(短头 → 客户端按明文保存)。
typedef struct {
    uint8_t nonce[16];
    uint8_t wrapped_key[SC_WRAPPED_KEY_LEN];
    uint8_t session_nonce[16];
} sc_file_keys_t;

size_t sc_build_file_head(uint8_t *out, size_t cap, uint32_t file_id, uint32_t size,
                          const sc_file_keys_t *keys, uint8_t error_code);

// 文件分片(1A08)载荷:n 个单元,单元 i 的序号为 first_seq+i,数据为 frames[i](长度 lens[i]
// ≤ 160,尾部补零)。返回载荷长度;cap 不足返回 0。
size_t sc_build_slices(uint8_t *out, size_t cap, uint32_t first_seq,
                       const uint8_t *const *frames, const uint8_t *lens, size_t n);

// Wi-Fi 1A05 应答:ip 为点分顺序(如 {192,168,4,1}),线上按客户端解析规则倒序写出。
size_t sc_build_wifi_endpoint(uint8_t *out, size_t cap, const uint8_t ip[4], uint16_t port);

// ---- 命令载荷解析 ----
typedef struct {
    uint32_t transferred;       // 已传输字节
    uint32_t file_id;
    bool realtime;
} sc_transfer_req_t;

bool sc_parse_transfer_req(const sc_cmd_t *cmd, sc_transfer_req_t *out);

typedef struct {
    char ssid[33];
    char password[65];
} sc_wifi_creds_t;

bool sc_parse_wifi_creds(const sc_cmd_t *cmd, sc_wifi_creds_t *out);

// ---- 杂项 ----
// SOC 百分比(-1 = 不可用)→ D3200 的 0..9 电量档。
uint8_t sc_battery_bucket(int soc);
// 十六进制文本 → 字节;忽略非十六进制字符。返回字节数,奇数位或溢出返回 0。
size_t sc_hex_decode(const char *text, size_t len, uint8_t *out, size_t cap);

static inline uint32_t sc_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint16_t sc_rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static inline void sc_wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline void sc_wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
