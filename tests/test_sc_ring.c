// tests/test_sc_ring.c —— 最近帧环形缓冲的 host 测试:与"全部帧"模型逐帧比对。
#include "sc_ring.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define TOTAL 20000

static uint8_t model[TOTAL][SC_RING_FRAME_MAX];
static uint8_t model_len[TOTAL];
static sc_ring_t ring;

static uint32_t rng_state = 12345;
static uint32_t rnd(void)
{
    rng_state = rng_state * 1103515245u + 12345u;
    return rng_state >> 8;
}

int main(void)
{
    sc_ring_reset(&ring, 0);
    uint32_t prev_first = 0;
    for (uint32_t i = 0; i < TOTAL; i++) {
        // 大多是 30~60 字节的 SILK 帧,偶尔出现满 160 的大帧
        size_t len = (rnd() % 16 == 0) ? SC_RING_FRAME_MAX : 30 + rnd() % 31;
        for (size_t k = 0; k < len; k++) model[i][k] = (uint8_t)(i * 7 + k);
        model_len[i] = (uint8_t)len;

        uint32_t predicted = sc_ring_first_after_push(&ring, len);
        uint32_t seq = sc_ring_push(&ring, model[i], len);
        assert(seq == i);
        assert(ring.first_seq == predicted);    // 预测与实际淘汰一致
        assert(ring.first_seq >= prev_first);   // 只会淘汰最旧的前缀
        assert(ring.first_seq <= seq);          // 新帧本身一定在环内
        prev_first = ring.first_seq;

        // 环内的每一帧内容都必须完整无损
        if (i % 97 == 0 || i == TOTAL - 1) {
            for (uint32_t s = ring.first_seq; s < ring.next_seq; s++) {
                uint8_t buf[SC_RING_FRAME_MAX];
                uint8_t l = 0;
                assert(sc_ring_get(&ring, s, buf, &l));
                assert(l == model_len[s]);
                assert(memcmp(buf, model[s], l) == 0);
            }
        }
        // 环里至少保留约 1 秒(50 帧)的音频,实时传输才有余量
        if (i >= 200) assert(ring.next_seq - ring.first_seq >= 50);
    }
    uint8_t buf[SC_RING_FRAME_MAX], l;
    assert(!sc_ring_get(&ring, ring.first_seq - 1, buf, &l));
    assert(!sc_ring_get(&ring, ring.next_seq, buf, &l));
    puts("test_sc_ring: PASS");
    return 0;
}
