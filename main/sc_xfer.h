// main/sc_xfer.h —— 文件传输帧构造(1A07 文件头 / 1A08 分片 / 1A0A 完成)。
//
// BLE 与 Wi-Fi 两条传输路径共用;只依赖 sc_proto 与 sc_crypto,可在 host 上测试。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sc_proto.h"

#define SC_XFER_MAX_UNITS 32u     // 单个分片帧最多单元数

typedef struct {
    bool active;
    uint8_t key[32];            // HKDF 派生的会话密钥
} sc_session_t;

typedef struct {
    uint32_t file_id;
    uint32_t size;              // 字节(帧数 x 160)
    uint8_t file_key[32];
    uint8_t nonce[16];
    bool encrypt;               // 由 sc_xfer_head_frame 决定:有会话才加密
} sc_xfer_file_t;

// 文件头帧。有会话:87 字节载荷(含包裹后的文件密钥),之后的分片加密;
// 无会话:8 字节短头,客户端按明文保存。error_code 非 0 时(如 1=文件不存在)总写长头。
size_t sc_xfer_head_frame(uint8_t *out, size_t cap, const sc_session_t *session, sc_xfer_file_t *file,
                          uint8_t error_code);

// 分片帧:n 个单元(序号 first_seq 起),按需加密。返回帧长,cap 不足返回 0。
size_t sc_xfer_slice_frame(uint8_t *out, size_t cap, const sc_xfer_file_t *file, uint32_t first_seq,
                           const uint8_t (*bufs)[SC_UNIT_DATA], const uint8_t *lens, size_t n);

// 完成帧(载荷为 fileId)。
size_t sc_xfer_done_frame(uint8_t *out, size_t cap, uint32_t file_id);

static inline size_t sc_xfer_slice_frame_size(size_t n) {
    return SC_FRAME_OVERHEAD + n * SC_UNIT_SIZE;
}
