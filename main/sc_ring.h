// main/sc_ring.h —— 正在录制文件的最近若干 Opus 帧(纯逻辑,host 可测)。
//
// 实时传输(1A07 realtimeFlag=1)需要读到刚编码出的帧;直接从 LittleFS 读要求
// 每帧 fsync,既慢又磨损闪存。环形缓冲保存最近的帧,文件只需周期性同步。
// 帧按序号连续追加;容量不足时淘汰最旧的帧。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SC_RING_BYTES  8192u
#define SC_RING_SLOTS  256u     // 2 的幂
#define SC_RING_FRAME_MAX 160u

typedef struct {
    uint8_t data[SC_RING_BYTES];
    uint16_t off[SC_RING_SLOTS];
    uint8_t len[SC_RING_SLOTS];
    uint32_t first_seq;         // 最旧帧序号
    uint32_t next_seq;          // 下一帧序号(= 已追加帧总数 + 起始序号)
    size_t head;                // 下一帧写入字节位置
} sc_ring_t;

void sc_ring_reset(sc_ring_t *r, uint32_t start_seq);

// 若追加下一帧需要淘汰帧,返回将被淘汰的最大序号 + 1(即淘汰后新的 first_seq),
// 否则返回当前 first_seq。调用方据此判断淘汰前是否需要先把文件同步到该序号。
uint32_t sc_ring_first_after_push(const sc_ring_t *r, size_t len);

// 追加一帧(len <= SC_RING_FRAME_MAX)。返回该帧的序号。
uint32_t sc_ring_push(sc_ring_t *r, const uint8_t *frame, size_t len);

// 读取 seq 帧;不在环内返回 false。
bool sc_ring_get(const sc_ring_t *r, uint32_t seq, uint8_t *out, uint8_t *len);

static inline bool sc_ring_contains(const sc_ring_t *r, uint32_t seq) {
    return seq >= r->first_seq && seq < r->next_seq;
}
