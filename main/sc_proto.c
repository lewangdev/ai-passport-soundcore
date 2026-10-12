// main/sc_proto.c —— soundcore Work 线协议实现(纯逻辑)。
#include "sc_proto.h"

#include <string.h>

static const uint8_t TX_MAGIC[5] = { 0x08, 0xEE, 0x00, 0x00, 0x00 };

uint8_t sc_checksum(const uint8_t *data, size_t len)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) sum += data[i];
    return (uint8_t)sum;
}

void sc_rx_init(sc_rx_t *rx, bool require_checksum)
{
    rx->len = 0;
    rx->require_checksum = require_checksum;
    rx->dropped_bytes = 0;
}

static void rx_drop(sc_rx_t *rx, size_t n)
{
    if (n >= rx->len) {
        rx->len = 0;
        return;
    }
    memmove(rx->buf, rx->buf + n, rx->len - n);
    rx->len -= n;
}

// 丢弃缓冲头部直到出现 magic 的可能起点(08),保留可能不完整的 magic 前缀。
static void rx_resync(sc_rx_t *rx)
{
    size_t i = 0;
    while (i < rx->len) {
        size_t n = rx->len - i < sizeof(TX_MAGIC) ? rx->len - i : sizeof(TX_MAGIC);
        if (memcmp(rx->buf + i, TX_MAGIC, n) == 0) break;
        i++;
    }
    rx->dropped_bytes += (uint32_t)i;
    rx_drop(rx, i);
}

size_t sc_rx_feed(sc_rx_t *rx, const uint8_t *data, size_t len, sc_cmd_cb_t cb, void *ctx)
{
    size_t delivered = 0;
    while (len > 0) {
        size_t room = sizeof(rx->buf) - rx->len;
        size_t n = len < room ? len : room;
        memcpy(rx->buf + rx->len, data, n);
        rx->len += n;
        data += n;
        len -= n;

        for (;;) {
            rx_resync(rx);
            if (rx->len < SC_HDR_LEN) break;
            uint16_t total = sc_rd16(rx->buf + 7);
            if (total < SC_FRAME_OVERHEAD || total > sizeof(rx->buf)) {
                // 长度不合理:跳过这个 magic,重新找下一个。
                rx->dropped_bytes++;
                rx_drop(rx, 1);
                continue;
            }
            if (rx->len < total) break;
            bool ok = sc_checksum(rx->buf, total - 1u) == rx->buf[total - 1u];
            if (ok || !rx->require_checksum) {
                sc_cmd_t cmd = {
                    .type = rx->buf[5],
                    .id = rx->buf[6],
                    .payload = rx->buf + SC_HDR_LEN,
                    .payload_len = total - SC_FRAME_OVERHEAD,
                    .checksum_ok = ok,
                };
                if (cb) cb(&cmd, ctx);
                delivered++;
                rx_drop(rx, total);
            } else {
                rx->dropped_bytes++;
                rx_drop(rx, 1);
            }
        }
        // 缓冲已满却仍凑不出帧(不可能出现的超长帧):整体丢弃,防止卡死。
        if (rx->len == sizeof(rx->buf)) {
            rx->dropped_bytes += (uint32_t)rx->len;
            rx->len = 0;
        }
    }
    return delivered;
}

size_t sc_frame_build(uint8_t *out, size_t cap, uint8_t status, uint8_t type, uint8_t id,
                      const uint8_t *payload, size_t plen)
{
    size_t total = SC_FRAME_OVERHEAD + plen;
    if (total > 0xFFFFu || total > cap) return 0;
    out[0] = 0x09;
    out[1] = 0xFF;
    out[2] = 0x00;
    out[3] = 0x00;
    out[4] = status;
    out[5] = type;
    out[6] = id;
    sc_wr16(out + 7, (uint16_t)total);
    if (plen) memmove(out + SC_HDR_LEN, payload, plen);
    out[total - 1u] = sc_checksum(out, total - 1u);
    return total;
}

static void put_ascii(uint8_t *dst, const char *src, size_t width)
{
    size_t n = src ? strlen(src) : 0;
    if (n > width) n = width;
    memset(dst, 0, width);
    if (n) memcpy(dst, src, n);
}

size_t sc_build_devinfo(uint8_t *out, size_t cap, const sc_devinfo_t *info)
{
    if (cap < SC_DEVINFO_LEN) return 0;
    uint8_t *p = out;
    *p++ = 1;                                   // connectStatus
    *p++ = info->battery_bucket;
    *p++ = info->charging ? 1 : 0;
    put_ascii(p, info->firmware, 5); p += 5;
    put_ascii(p, info->serial, 16); p += 16;
    sc_wr32(p, info->total_kb); p += 4;
    sc_wr32(p, info->free_kb); p += 4;
    *p++ = info->box_charging ? 1 : 0;
    put_ascii(p, info->box_firmware, 5); p += 5;
    *p++ = info->box_battery_bucket;
    memcpy(p, info->box_mac, 6); p += 6;
    *p++ = info->color;
    *p++ = 0;                                   // autoPowerOffSwitch
    *p++ = 0;                                   // autoPowerOffIndex
    *p++ = 1;                                   // pickupIndicatorLight
    *p++ = 0;                                   // boxIndicatorLight
    *p++ = info->record_status;
    put_ascii(p, info->wifi_firmware, 5); p += 5;
    // 保留字节:客户端只在 wifiFirmware 之后仍有数据时才解析它(o + 5 <= len - 1)。
    *p++ = 0;
    return (size_t)(p - out);
}

size_t sc_build_file_list(uint8_t *out, size_t cap, const sc_file_entry_t *entries, size_t count,
                          uint16_t page, bool with_end, uint32_t current_id, int32_t current_dur)
{
    size_t first = (size_t)page * SC_FILE_PAGE_SIZE;
    size_t n = first < count ? count - first : 0;
    if (n > SC_FILE_PAGE_SIZE) n = SC_FILE_PAGE_SIZE;
    size_t entry_size = with_end ? 12u : 8u;
    size_t need = 2u + n * entry_size + 8u;
    if (need > cap) return 0;

    uint8_t *p = out;
    sc_wr16(p, (uint16_t)n); p += 2;
    for (size_t i = 0; i < n; i++) {
        const sc_file_entry_t *e = &entries[first + i];
        sc_wr32(p, e->file_id); p += 4;
        if (with_end) { sc_wr32(p, e->end_time); p += 4; }
        sc_wr32(p, e->size); p += 4;
    }
    sc_wr32(p, current_id); p += 4;
    sc_wr32(p, (uint32_t)current_dur); p += 4;
    return (size_t)(p - out);
}

size_t sc_build_file_head(uint8_t *out, size_t cap, uint32_t file_id, uint32_t size,
                          const sc_file_keys_t *keys, uint8_t error_code)
{
    size_t need = keys ? SC_FILE_HEAD_LEN : 8u;
    if (need > cap) return 0;
    sc_wr32(out, file_id);
    sc_wr32(out + 4, size);
    if (!keys) return 8u;
    memcpy(out + 8, keys->nonce, 16);
    memcpy(out + 24, keys->wrapped_key, SC_WRAPPED_KEY_LEN);
    memcpy(out + 70, keys->session_nonce, 16);
    out[86] = error_code;
    return SC_FILE_HEAD_LEN;
}

size_t sc_build_slices(uint8_t *out, size_t cap, uint32_t first_seq,
                       const uint8_t *const *frames, const uint8_t *lens, size_t n)
{
    if (n * SC_UNIT_SIZE > cap) return 0;
    uint8_t *p = out;
    for (size_t i = 0; i < n; i++) {
        size_t len = lens[i] > SC_UNIT_DATA ? SC_UNIT_DATA : lens[i];
        sc_wr32(p, first_seq + (uint32_t)i);
        p[4] = 0;                               // flags
        memcpy(p + 5, frames[i], len);
        memset(p + 5 + len, 0, SC_UNIT_DATA - len);
        p[5 + SC_UNIT_DATA] = 0;                // pad
        p += SC_UNIT_SIZE;
    }
    return (size_t)(p - out);
}

size_t sc_build_wifi_endpoint(uint8_t *out, size_t cap, const uint8_t ip[4], uint16_t port)
{
    if (cap < 6) return 0;
    // 客户端:ip = data[12].data[11].data[10].data[9] → 线上从最后一段开始写。
    out[0] = ip[3];
    out[1] = ip[2];
    out[2] = ip[1];
    out[3] = ip[0];
    sc_wr16(out + 4, port);
    return 6;
}

bool sc_parse_transfer_req(const sc_cmd_t *cmd, sc_transfer_req_t *out)
{
    if (cmd->type != SC_T_FILE || cmd->id != SC_ID_FILE_HEAD || cmd->payload_len < 8) return false;
    out->transferred = sc_rd32(cmd->payload);
    out->file_id = sc_rd32(cmd->payload + 4);
    // BLE 请求带 realtimeFlag;Wi-Fi 文本请求只有 8 字节载荷(末字节 0x1B 占了校验位)。
    out->realtime = cmd->payload_len >= 9 && cmd->payload[8] == 1;
    return true;
}

bool sc_parse_wifi_creds(const sc_cmd_t *cmd, sc_wifi_creds_t *out)
{
    const uint8_t *p = cmd->payload;
    size_t left = cmd->payload_len;
    if (left < 1) return false;
    size_t ssid_len = p[0];
    if (ssid_len == 0 || ssid_len > 32 || left < 1 + ssid_len + 1) return false;
    memcpy(out->ssid, p + 1, ssid_len);
    out->ssid[ssid_len] = '\0';
    p += 1 + ssid_len;
    left -= 1 + ssid_len;
    size_t pwd_len = p[0];
    if (pwd_len > 64 || left < 1 + pwd_len) return false;
    memcpy(out->password, p + 1, pwd_len);
    out->password[pwd_len] = '\0';
    // WPA2 要求 8..63 字符;0 表示开放热点。
    if (pwd_len != 0 && pwd_len < 8) return false;
    return true;
}

uint8_t sc_battery_bucket(int soc)
{
    if (soc < 0) return 9;
    if (soc > 100) soc = 100;
    int bucket = (soc + 5) / 10 - 1;
    if (bucket < 0) bucket = 0;
    if (bucket > 9) bucket = 9;
    return (uint8_t)bucket;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

size_t sc_hex_decode(const char *text, size_t len, uint8_t *out, size_t cap)
{
    size_t n = 0;
    int hi = -1;
    for (size_t i = 0; i < len; i++) {
        int v = hex_val(text[i]);
        if (v < 0) continue;
        if (hi < 0) {
            hi = v;
            continue;
        }
        if (n >= cap) return 0;
        out[n++] = (uint8_t)((hi << 4) | v);
        hi = -1;
    }
    return hi < 0 ? n : 0;
}
