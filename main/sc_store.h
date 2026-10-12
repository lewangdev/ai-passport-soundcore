// main/sc_store.h —— 录音文件存储(LittleFS,分区 "storage")。
//
// 每条录音:
//   /rec/<id>.dat   96 字节头(magic、id、起止时间、帧数、文件密钥、nonce)+ 帧记录 [len u8][opus]
//   /rec/<id>.idx   每 SC_STORE_IDX_STRIDE 帧一个 u32 文件偏移,用于断点续传时定位
// 对外报告的文件大小 = 帧数 x 160(线上每帧固定 160 字节单元)。
// 所有函数线程安全(内部互斥);写入只来自录音任务,读取来自传输任务/HTTP 任务。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "sc_proto.h"

#define SC_STORE_MAX_FILES   200u
#define SC_STORE_IDX_STRIDE  256u
#define SC_STORE_MIN_FREE_KB 96u    // 低于该剩余空间拒绝开始/自动停止录音

typedef struct {
    uint32_t file_id;
    uint32_t start_time;
    uint32_t end_time;
    uint32_t frames;
    uint8_t file_key[32];
    uint8_t nonce[16];
    bool live;                  // 正在录制(或暂停中)
} sc_store_info_t;

esp_err_t sc_store_init(void);

// 文件清单,新的在前。返回条数。
size_t sc_store_list(sc_file_entry_t *out, size_t cap);
size_t sc_store_count(void);
bool sc_store_get(uint32_t file_id, sc_store_info_t *out);
void sc_store_usage(uint32_t *total_kb, uint32_t *free_kb);

// ---- 录制(仅录音任务调用)----
esp_err_t sc_store_begin(uint32_t file_id, uint32_t start_time);
esp_err_t sc_store_append(const uint8_t *frame, uint8_t len);
// 把已写入的帧落盘(暂停时调用,断电不丢暂停前的录音)。
void sc_store_sync(void);
// 结束当前录音并落盘头部。
esp_err_t sc_store_end(uint32_t end_time);
uint32_t sc_store_live_id(void);       // 0 = 无
uint32_t sc_store_live_frames(void);

// ---- 读取 ----
// 一个读者顺序读一条录音;同一时刻最多 SC_STORE_READERS 个。
#define SC_STORE_READERS 2
typedef struct sc_reader sc_reader_t;

sc_reader_t *sc_store_reader_open(uint32_t file_id);
// 从 seq 起读最多 max 帧到 bufs(每帧 160 字节槽位)与 lens;返回实际帧数
// (0 = 已到当前末尾;<0 = 错误)。
int sc_store_reader_read(sc_reader_t *rd, uint32_t seq, uint8_t (*bufs)[SC_UNIT_DATA],
                         uint8_t *lens, int max);
void sc_store_reader_close(sc_reader_t *rd);

// ---- 管理 ----
esp_err_t sc_store_delete(uint32_t file_id);
esp_err_t sc_store_wipe(void);
