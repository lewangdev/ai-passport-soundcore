// tests/sc_device_harness.c —— 把固件的协议/加密模块包成命令行"设备",供
// tests/test_sc_client_compat.py 用客户端逻辑(Python 移植)端到端校验。
//
//   sc_device_harness session <app_pub_hex>   握手 + 一个加密文件的完整传输
//   sc_device_harness plain                   无会话(明文)传输
//   sc_device_harness parse <hex>             解析手机命令帧
// 输出逐行:KIND <hex>,KIND ∈ {FRAME, PLAIN, CMD}。
#include "sc_crypto.h"
#include "sc_proto.h"
#include "sc_xfer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FILE_ID 1735689601u
#define FRAMES  123u

static void print_hex(const char *kind, const uint8_t *p, size_t n)
{
    printf("%s ", kind);
    for (size_t i = 0; i < n; i++) printf("%02x", p[i]);
    putchar('\n');
}

static uint32_t rng = 0xC0FFEEu;
static uint8_t rnd8(void)
{
    rng = rng * 1664525u + 1013904223u;
    return (uint8_t)(rng >> 24);
}

// 伪 Opus 帧:长度 1..160,末字节非零(SILK 帧在编码器侧已去掉尾零)。
static void make_frames(uint8_t frames[FRAMES][SC_UNIT_DATA], uint8_t lens[FRAMES])
{
    for (uint32_t i = 0; i < FRAMES; i++) {
        uint8_t len = (uint8_t)(i % 17 == 0 ? 160 : 20 + rnd8() % 60);
        for (uint8_t k = 0; k < len; k++) frames[i][k] = rnd8();
        if (frames[i][len - 1] == 0) frames[i][len - 1] = 0x5A;
        lens[i] = len;
        print_hex("PLAIN", frames[i], len);
    }
}

static void transfer(const sc_session_t *session)
{
    static uint8_t frames[FRAMES][SC_UNIT_DATA];
    static uint8_t lens[FRAMES];
    make_frames(frames, lens);

    sc_xfer_file_t file = { .file_id = FILE_ID, .size = FRAMES * SC_UNIT_DATA };
    sc_crypto_random(file.file_key, sizeof(file.file_key));
    sc_crypto_random(file.nonce, sizeof(file.nonce));

    static uint8_t out[SC_FRAME_OVERHEAD + SC_XFER_MAX_UNITS * SC_UNIT_SIZE];
    size_t n = sc_xfer_head_frame(out, sizeof(out), session, &file, 0);
    print_hex("FRAME", out, n);
    for (uint32_t seq = 0; seq < FRAMES;) {
        size_t k = FRAMES - seq < 6 ? FRAMES - seq : 6;
        n = sc_xfer_slice_frame(out, sizeof(out), &file, seq, (const uint8_t (*)[SC_UNIT_DATA])&frames[seq],
                                &lens[seq], k);
        if (n == 0) exit(2);
        print_hex("FRAME", out, n);
        seq += (uint32_t)k;
    }
    n = sc_xfer_done_frame(out, sizeof(out), FILE_ID);
    print_hex("FRAME", out, n);
}

static void on_cmd(const sc_cmd_t *cmd, void *ctx)
{
    (void)ctx;
    uint8_t buf[300];
    buf[0] = cmd->type;
    buf[1] = cmd->id;
    buf[2] = cmd->checksum_ok;
    memcpy(buf + 3, cmd->payload, cmd->payload_len);
    print_hex("CMD", buf, cmd->payload_len + 3);
}

int main(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "session") == 0) {
        uint8_t app_pub[SC_PUBKEY_LEN];
        if (sc_hex_decode(argv[2], strlen(argv[2]), app_pub, sizeof(app_pub)) != SC_PUBKEY_LEN) return 2;
        uint8_t reply[SC_PUBKEY_LEN + SC_KEY_LEN];
        sc_session_t session = { .active = true };
        if (sc_crypto_handshake(app_pub, reply, reply + SC_PUBKEY_LEN, session.key) != 0) return 3;
        uint8_t frame[128];
        size_t n = sc_frame_build(frame, sizeof(frame), 1, SC_T_ENCRYPT, SC_ID_ENCRYPT_KEY, reply, sizeof(reply));
        print_hex("FRAME", frame, n);
        transfer(&session);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "plain") == 0) {
        transfer(NULL);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "parse") == 0) {
        uint8_t bin[512];
        size_t n = sc_hex_decode(argv[2], strlen(argv[2]), bin, sizeof(bin));
        sc_rx_t rx;
        sc_rx_init(&rx, argc >= 4 && strcmp(argv[3], "lenient") == 0 ? false : true);
        sc_rx_feed(&rx, bin, n, on_cmd, NULL);
        return 0;
    }
    fprintf(stderr, "usage: %s session <hex> | plain | parse <hex> [lenient]\n", argv[0]);
    return 1;
}
