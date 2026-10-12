// tests/test_sc_proto.c —— soundcore Work 线协议的 host 测试。
// 偏移量断言对照客户端实现(tacshi/Soundcore lib/protocol、lib/ble、lib/wifi)。
#include "sc_proto.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    int count;
    uint8_t type[8], id[8];
    uint8_t payload[8][64];
    size_t plen[8];
    bool ok[8];
} sink_t;

static void collect(const sc_cmd_t *cmd, void *ctx)
{
    sink_t *s = ctx;
    assert(s->count < 8);
    s->type[s->count] = cmd->type;
    s->id[s->count] = cmd->id;
    s->plen[s->count] = cmd->payload_len;
    s->ok[s->count] = cmd->checksum_ok;
    memcpy(s->payload[s->count], cmd->payload, cmd->payload_len);
    s->count++;
}

// 与客户端 ProtocolFrame.encode 相同的命令编码。
static size_t encode_cmd(uint8_t *out, uint8_t type, uint8_t id, const uint8_t *p, size_t n)
{
    static const uint8_t magic[5] = { 0x08, 0xEE, 0, 0, 0 };
    size_t total = 10 + n;
    memcpy(out, magic, 5);
    out[5] = type;
    out[6] = id;
    out[7] = (uint8_t)total;
    out[8] = (uint8_t)(total >> 8);
    if (n) memcpy(out + 9, p, n);
    out[total - 1] = sc_checksum(out, total - 1);
    return total;
}

static void test_checksum_and_frame(void)
{
    uint8_t out[64];
    const uint8_t payload[2] = { 0x09, 0x09 };
    size_t n = sc_frame_build(out, sizeof(out), 1, 0x01, 0x03, payload, 2);
    assert(n == 12);
    assert(out[0] == 0x09 && out[1] == 0xFF && out[2] == 0 && out[3] == 0);
    assert(out[4] == 1 && out[5] == 0x01 && out[6] == 0x03);
    assert(sc_rd16(out + 7) == 12);
    assert(out[9] == 9 && out[10] == 9);
    assert(out[11] == sc_checksum(out, 11));
    assert(sc_frame_build(out, 11, 1, 1, 1, payload, 2) == 0);  // 容量不足
    // 客户端 _buildTransferCompleteHex 的已知帧:08 EE 00 00 00 1A 0A 0B 00 01 | csum
    const uint8_t done[10] = { 0x08, 0xEE, 0, 0, 0, 0x1A, 0x0A, 0x0B, 0x00, 0x01 };
    assert(sc_checksum(done, 10) == (uint8_t)(0x08 + 0xEE + 0x1A + 0x0A + 0x0B + 0x01));
}

static void test_rx_split_merge_and_garbage(void)
{
    uint8_t stream[128];
    size_t n = 0;
    stream[n++] = 0x55;                         // 前导垃圾
    stream[n++] = 0x08;                         // 假 magic 开头
    const uint8_t page[2] = { 0, 0 };
    n += encode_cmd(stream + n, 0x1B, 0x0E, page, 2);
    n += encode_cmd(stream + n, 0x01, 0x01, NULL, 0);
    const uint8_t rec[1] = { 1 };
    n += encode_cmd(stream + n, 0x18, 0x82, rec, 1);

    // 逐字节喂入(模拟 20 字节 MTU 以下的任意分片)
    sc_rx_t rx;
    sc_rx_init(&rx, true);
    sink_t s = { 0 };
    for (size_t i = 0; i < n; i++) sc_rx_feed(&rx, stream + i, 1, collect, &s);
    assert(s.count == 3);
    assert(s.type[0] == 0x1B && s.id[0] == 0x0E && s.plen[0] == 2);
    assert(s.type[1] == 0x01 && s.id[1] == 0x01 && s.plen[1] == 0);
    assert(s.type[2] == 0x18 && s.id[2] == 0x82 && s.plen[2] == 1 && s.payload[2][0] == 1);

    // 一次性喂入
    sc_rx_init(&rx, true);
    memset(&s, 0, sizeof(s));
    assert(sc_rx_feed(&rx, stream, n, collect, &s) == 3);

    // 校验和错误:严格模式丢弃,宽松模式交付并标记
    uint8_t bad[16];
    size_t bl = encode_cmd(bad, 0x01, 0x01, NULL, 0);
    bad[bl - 1] ^= 0xFF;
    sc_rx_init(&rx, true);
    memset(&s, 0, sizeof(s));
    sc_rx_feed(&rx, bad, bl, collect, &s);
    assert(s.count == 0);
    sc_rx_init(&rx, false);
    sc_rx_feed(&rx, bad, bl, collect, &s);
    assert(s.count == 1 && !s.ok[0]);
}

static void test_ws_text_transfer_request(void)
{
    // 客户端 _buildFileHeaderRequestHex:18 字节,无校验和(末字节 0x1B)。
    const char *hex = "08ee0000001a071200" "00a00000" "01276567" "1b";
    uint8_t bin[32];
    size_t n = sc_hex_decode(hex, strlen(hex), bin, sizeof(bin));
    assert(n == 18);
    sc_rx_t rx;
    sc_rx_init(&rx, false);
    sink_t s = { 0 };
    sc_rx_feed(&rx, bin, n, collect, &s);
    assert(s.count == 1 && s.type[0] == 0x1A && s.id[0] == 0x07 && s.plen[0] == 8);
    sc_cmd_t cmd = { .type = 0x1A, .id = 0x07, .payload = s.payload[0], .payload_len = 8 };
    sc_transfer_req_t req;
    assert(sc_parse_transfer_req(&cmd, &req));
    assert(req.transferred == 0xA000 && req.file_id == 0x67652701 && !req.realtime);

    // BLE 版本带 realtimeFlag
    uint8_t p[9];
    sc_wr32(p, 320);
    sc_wr32(p + 4, 1735689601u);
    p[8] = 1;
    cmd.payload = p;
    cmd.payload_len = 9;
    assert(sc_parse_transfer_req(&cmd, &req) && req.realtime && req.transferred == 320);
    assert(sc_hex_decode("abc", 3, bin, sizeof(bin)) == 0);   // 奇数位
}

static void test_devinfo_layout(void)
{
    sc_devinfo_t info = {
        .battery_bucket = 7, .charging = false, .firmware = "01.00", .serial = "apd3aabbccddeeff",
        .total_kb = 5000, .free_kb = 4000, .box_firmware = "01.00", .box_battery_bucket = 7,
        .box_mac = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xF0 }, .record_status = SC_REC_RECORDING,
        .wifi_firmware = "01.00",
    };
    uint8_t payload[SC_DEVINFO_LEN];
    assert(sc_build_devinfo(payload, sizeof(payload), &info) == SC_DEVINFO_LEN);
    uint8_t f[80];
    size_t n = sc_frame_build(f, sizeof(f), 1, 0x01, 0x01, payload, sizeof(payload));
    // DeviceInfoModel.parsePacket 的绝对偏移(从整帧 9 开始)
    assert(f[9] == 1);                          // connectStatus
    assert(f[10] == 7);                         // battery bucket
    assert(memcmp(f + 12, "01.00", 5) == 0);    // firmware
    assert(memcmp(f + 17, "apd3aabbccddeeff", 16) == 0);
    assert(sc_rd32(f + 33) == 5000 && sc_rd32(f + 37) == 4000);
    assert(memcmp(f + 42, "01.00", 5) == 0);    // box firmware
    assert(f[47] == 7);                         // box battery
    assert(f[48] == 0xAA && f[53] == 0xF0);     // box MAC
    assert(f[59] == SC_REC_RECORDING);          // recording(color+4 个设置字节之后)
    assert(memcmp(f + 60, "01.00", 5) == 0);    // wifi firmware
    // 客户端条件:o + 5 <= raw.length - 1 才解析 wifiFirmware(o=60)
    assert(60 + 5 <= (int)n - 1);
}

static void test_file_list_paging(void)
{
    sc_file_entry_t e[23];
    for (int i = 0; i < 23; i++) e[i] = (sc_file_entry_t){ 1735689600u + 100 - i, 1735689700u, (uint32_t)(i + 1) * 160 };
    uint8_t p[160];
    size_t n = sc_build_file_list(p, sizeof(p), e, 23, 0, true, 1735689700u, 42);
    assert(n == 2 + 10 * 12 + 8);
    assert(sc_rd16(p) == 10);
    assert(sc_rd32(p + 2) == e[0].file_id && sc_rd32(p + 6) == e[0].end_time && sc_rd32(p + 10) == e[0].size);
    assert(sc_rd32(p + 2 + 120) == 1735689700u && sc_rd32(p + 2 + 124) == 42);
    n = sc_build_file_list(p, sizeof(p), e, 23, 2, true, 0, 0);
    assert(sc_rd16(p) == 3 && n == 2 + 3 * 12 + 8);
    assert(sc_rd32(p + 2) == e[20].file_id);
    n = sc_build_file_list(p, sizeof(p), e, 23, 3, true, 0, 0);
    assert(sc_rd16(p) == 0 && n == 10);
    // 1A0E:每条 8 字节(id + size)
    n = sc_build_file_list(p, sizeof(p), e, 23, 0, false, 0, 0);
    assert(n == 2 + 80 + 8 && sc_rd32(p + 6) == e[0].size);
}

static void test_file_head_and_slices(void)
{
    sc_file_keys_t keys;
    for (int i = 0; i < 16; i++) keys.nonce[i] = (uint8_t)i;
    for (int i = 0; i < 46; i++) keys.wrapped_key[i] = (uint8_t)(0x40 + i);
    for (int i = 0; i < 16; i++) keys.session_nonce[i] = (uint8_t)(0x90 + i);
    uint8_t p[SC_FILE_HEAD_LEN];
    size_t n = sc_build_file_head(p, sizeof(p), 1735689601u, 3200, &keys, 0);
    uint8_t f[128];
    size_t fl = sc_frame_build(f, sizeof(f), 1, 0x1A, 0x07, p, n);
    // AudioFileSecretKey.parseFrame:总长 >= 97,id@9 size@13 nonce@17 key@33 sessNonce@79 err@95
    assert(fl == 97);
    assert(sc_rd32(f + 9) == 1735689601u && sc_rd32(f + 13) == 3200);
    assert(f[17] == 0 && f[32] == 15);
    assert(f[33] == 0x40 && f[78] == 0x40 + 45);
    assert(f[79] == 0x90 && f[94] == 0x9F);
    assert(f[95] == 0);
    assert(sc_build_file_head(p, sizeof(p), 1, 2, NULL, 0) == 8);

    uint8_t a[160], b[3] = { 1, 2, 3 };
    memset(a, 0x77, sizeof(a));
    const uint8_t *frames[2] = { a, b };
    const uint8_t lens[2] = { 160, 3 };
    uint8_t sl[2 * SC_UNIT_SIZE];
    n = sc_build_slices(sl, sizeof(sl), 50, frames, lens, 2);
    assert(n == 2 * SC_UNIT_SIZE);
    uint8_t frame[400];
    fl = sc_frame_build(frame, sizeof(frame), 1, 0x1A, 0x08, sl, n);
    // 客户端 _writeSlices:从 9 开始,每单元 seq u32 | flags | 160 数据 | pad
    size_t i = 9;
    int units = 0;
    while (fl - i >= 165) {
        uint32_t seq = sc_rd32(frame + i);
        assert(seq == 50u + (uint32_t)units);
        const uint8_t *data = frame + i + 5;
        if (units == 0) assert(data[0] == 0x77 && data[159] == 0x77);
        if (units == 1) assert(data[0] == 1 && data[2] == 3 && data[3] == 0 && data[159] == 0);
        i += 165;
        if (i < fl) i += 1;
        units++;
    }
    assert(units == 2);
}

static void test_wifi_endpoint_and_creds(void)
{
    uint8_t p[8];
    const uint8_t ip[4] = { 192, 168, 4, 1 };
    assert(sc_build_wifi_endpoint(p, sizeof(p), ip, 443) == 6);
    uint8_t f[32];
    sc_frame_build(f, sizeof(f), 1, 0x1A, 0x05, p, 6);
    // 客户端:ip = data[12].data[11].data[10].data[9];port = data[13] | data[14] << 8
    assert(f[12] == 192 && f[11] == 168 && f[10] == 4 && f[9] == 1);
    assert((f[13] | (f[14] << 8)) == 443);

    uint8_t c[64];
    const char *ssid = "WiFi-1760000000000", *pwd = "1760000000000";
    size_t n = 0;
    c[n++] = (uint8_t)strlen(ssid);
    memcpy(c + n, ssid, strlen(ssid)); n += strlen(ssid);
    c[n++] = (uint8_t)strlen(pwd);
    memcpy(c + n, pwd, strlen(pwd)); n += strlen(pwd);
    sc_cmd_t cmd = { .type = 0x1A, .id = 0x05, .payload = c, .payload_len = n };
    sc_wifi_creds_t creds;
    assert(sc_parse_wifi_creds(&cmd, &creds));
    assert(strcmp(creds.ssid, ssid) == 0 && strcmp(creds.password, pwd) == 0);
    cmd.payload_len = n - 1;                    // 截断
    assert(!sc_parse_wifi_creds(&cmd, &creds));
    uint8_t shortpw[] = { 1, 'A', 3, '1', '2', '3' };   // WPA2 密码过短
    cmd.payload = shortpw;
    cmd.payload_len = sizeof(shortpw);
    assert(!sc_parse_wifi_creds(&cmd, &creds));
}

static void test_battery_bucket(void)
{
    // 客户端显示:(bucket + 1) * 10%
    assert(sc_battery_bucket(100) == 9);
    assert(sc_battery_bucket(95) == 9);
    assert(sc_battery_bucket(50) == 4);
    assert(sc_battery_bucket(14) == 0);
    assert(sc_battery_bucket(0) == 0);
    assert(sc_battery_bucket(-1) == 9);
}

int main(void)
{
    test_checksum_and_frame();
    test_rx_split_merge_and_garbage();
    test_ws_text_transfer_request();
    test_devinfo_layout();
    test_file_list_paging();
    test_file_head_and_slices();
    test_wifi_endpoint_and_creds();
    test_battery_bucket();
    puts("test_sc_proto: PASS");
    return 0;
}
