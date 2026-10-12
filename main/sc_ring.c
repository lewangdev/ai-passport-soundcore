// main/sc_ring.c —— 最近帧环形缓冲(纯逻辑)。
#include "sc_ring.h"

#include <string.h>

#define SLOT(seq) ((seq) & (SC_RING_SLOTS - 1u))

void sc_ring_reset(sc_ring_t *r, uint32_t start_seq)
{
    r->first_seq = start_seq;
    r->next_seq = start_seq;
    r->head = 0;
}

// 新帧的写入起点:放不下尾部空间就回绕到 0(帧数据始终连续)。
static size_t write_pos(const sc_ring_t *r, size_t len)
{
    return r->head + len <= SC_RING_BYTES ? r->head : 0;
}

// 帧按写入顺序首尾相接地排布,因此需要淘汰的总是"最旧的若干帧"这一前缀:
//  - 槽位已满;
//  - 回绕到 0 时,上一圈留在 head 之后的帧(它们最旧,且被跳过的尾部区域隔开);
//  - 与新帧字节区间重叠的帧。
static uint32_t first_after(const sc_ring_t *r, size_t len, size_t pos)
{
    bool wrapped = pos != r->head;
    uint32_t first = r->first_seq;
    while (first < r->next_seq) {
        size_t o = r->off[SLOT(first)];
        size_t l = r->len[SLOT(first)];
        bool slot_full = r->next_seq - first >= SC_RING_SLOTS;
        bool stale_tail = wrapped && o >= r->head;
        bool overlap = o < pos + len && pos < o + (l ? l : 1u);
        if (!slot_full && !stale_tail && !overlap) break;
        first++;
    }
    return first;
}

uint32_t sc_ring_first_after_push(const sc_ring_t *r, size_t len)
{
    if (len > SC_RING_FRAME_MAX) len = SC_RING_FRAME_MAX;
    return first_after(r, len, write_pos(r, len));
}

uint32_t sc_ring_push(sc_ring_t *r, const uint8_t *frame, size_t len)
{
    if (len > SC_RING_FRAME_MAX) len = SC_RING_FRAME_MAX;
    size_t pos = write_pos(r, len);
    r->first_seq = first_after(r, len, pos);
    uint32_t seq = r->next_seq++;
    r->off[SLOT(seq)] = (uint16_t)pos;
    r->len[SLOT(seq)] = (uint8_t)len;
    if (len) memcpy(r->data + pos, frame, len);
    r->head = pos + len;
    return seq;
}

bool sc_ring_get(const sc_ring_t *r, uint32_t seq, uint8_t *out, uint8_t *len)
{
    if (!sc_ring_contains(r, seq)) return false;
    size_t l = r->len[SLOT(seq)];
    memcpy(out, r->data + r->off[SLOT(seq)], l);
    *len = (uint8_t)l;
    return true;
}
