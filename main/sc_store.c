// main/sc_store.c —— 录音文件存储实现(LittleFS)。
#include "sc_store.h"

#include "sc_crypto.h"
#include "sc_ring.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_littlefs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "sc_store";

#define BASE_PATH     "/lfs"
#define REC_DIR       BASE_PATH "/rec"
#define PART_LABEL    "storage"
#define HDR_SIZE      96u
#define HDR_MAGIC     0x31524353u   // "SCR1"
#define HDR_VERSION   1u
#define FRAMES_OPEN   0xFFFFFFFFu
#define SYNC_EVERY    50u           // 约 1 秒同步一次文件
#define FRAMES_PER_S  50u

typedef struct {
    uint32_t file_id;
    uint32_t start_time;
    uint32_t end_time;
    uint32_t frames;
} cat_entry_t;

typedef struct {
    FILE *f;
    FILE *idx;
    uint32_t file_id;
    uint32_t start_time;
    uint32_t frames;
    uint32_t synced;            // 已 fsync 的帧数(其他句柄可读到)
    uint32_t pos;               // 下一条记录的文件偏移
    uint8_t file_key[32];
    uint8_t nonce[16];
} live_t;

struct sc_reader {
    bool used;
    uint32_t file_id;
    FILE *f;
    uint32_t cur_seq;           // f 当前位置对应的帧序号
    uint32_t visible;           // 打开时文件内可读的帧数
};

static SemaphoreHandle_t s_mutex;
static cat_entry_t s_cat[SC_STORE_MAX_FILES];
static size_t s_cat_count;
static live_t s_live;
static sc_ring_t s_ring;
static sc_reader_t s_readers[SC_STORE_READERS];
static bool s_ready;

static void lock(void) { xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGiveRecursive(s_mutex); }

static void path_dat(char *buf, size_t cap, uint32_t id) { snprintf(buf, cap, REC_DIR "/%lu.dat", (unsigned long)id); }
static void path_idx(char *buf, size_t cap, uint32_t id) { snprintf(buf, cap, REC_DIR "/%lu.idx", (unsigned long)id); }

static void hdr_encode(uint8_t h[HDR_SIZE], uint32_t id, uint32_t start, uint32_t end, uint32_t frames,
                       const uint8_t key[32], const uint8_t nonce[16])
{
    memset(h, 0, HDR_SIZE);
    sc_wr32(h + 0, HDR_MAGIC);
    sc_wr32(h + 4, HDR_VERSION);
    sc_wr32(h + 8, id);
    sc_wr32(h + 12, start);
    sc_wr32(h + 16, end);
    sc_wr32(h + 20, frames);
    memcpy(h + 24, key, 32);
    memcpy(h + 56, nonce, 16);
}

static bool hdr_read(const char *path, uint8_t h[HDR_SIZE])
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    bool ok = fread(h, 1, HDR_SIZE, f) == HDR_SIZE && sc_rd32(h) == HDR_MAGIC && sc_rd32(h + 4) == HDR_VERSION;
    fclose(f);
    return ok;
}

static void cat_sort(void)
{
    // 插入排序:条目少,且几乎总是有序。新的在前。
    for (size_t i = 1; i < s_cat_count; i++) {
        cat_entry_t e = s_cat[i];
        size_t j = i;
        while (j > 0 && s_cat[j - 1].file_id < e.file_id) {
            s_cat[j] = s_cat[j - 1];
            j--;
        }
        s_cat[j] = e;
    }
}

static cat_entry_t *cat_find(uint32_t id)
{
    for (size_t i = 0; i < s_cat_count; i++) {
        if (s_cat[i].file_id == id) return &s_cat[i];
    }
    return NULL;
}

static void cat_remove(uint32_t id)
{
    for (size_t i = 0; i < s_cat_count; i++) {
        if (s_cat[i].file_id == id) {
            memmove(&s_cat[i], &s_cat[i + 1], (s_cat_count - i - 1) * sizeof(s_cat[0]));
            s_cat_count--;
            return;
        }
    }
}

static void remove_files(uint32_t id)
{
    char p[48];
    path_dat(p, sizeof(p), id);
    unlink(p);
    path_idx(p, sizeof(p), id);
    unlink(p);
}

// 断电恢复:数出完整记录,重建索引,补写头部。返回帧数。
static uint32_t recover_file(uint32_t id, uint8_t h[HDR_SIZE])
{
    char p[48];
    path_dat(p, sizeof(p), id);
    FILE *f = fopen(p, "r+b");
    if (!f) return 0;
    path_idx(p, sizeof(p), id);
    FILE *idx = fopen(p, "wb");

    uint32_t frames = 0;
    uint32_t pos = HDR_SIZE;
    uint8_t rec[1 + SC_UNIT_DATA];
    fseek(f, HDR_SIZE, SEEK_SET);
    for (;;) {
        int len = fgetc(f);
        if (len == EOF || len > (int)SC_UNIT_DATA) break;
        if (len > 0 && fread(rec, 1, (size_t)len, f) != (size_t)len) break;
        if (idx && frames % SC_STORE_IDX_STRIDE == 0) {
            uint8_t b[4];
            sc_wr32(b, pos);
            fwrite(b, 1, 4, idx);
        }
        frames++;
        pos += 1u + (uint32_t)len;
    }
    if (idx) fclose(idx);
    uint32_t start = sc_rd32(h + 12);
    sc_wr32(h + 16, start + frames / FRAMES_PER_S);
    sc_wr32(h + 20, frames);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, HDR_SIZE, f);
    fclose(f);
    // 丢弃未完整写入的尾部记录不影响读取:读取按帧数截止。
    ESP_LOGW(TAG, "恢复未正常结束的录音 %lu:%lu 帧", (unsigned long)id, (unsigned long)frames);
    return frames;
}

static void scan_catalog(void)
{
    s_cat_count = 0;
    DIR *d = opendir(REC_DIR);
    if (!d) return;
    struct dirent *de;
    uint8_t h[HDR_SIZE];
    char p[300];
    while ((de = readdir(d)) != NULL) {
        size_t n = strlen(de->d_name);
        if (n < 5 || strcmp(de->d_name + n - 4, ".dat") != 0) continue;
        uint32_t id = (uint32_t)strtoul(de->d_name, NULL, 10);
        snprintf(p, sizeof(p), REC_DIR "/%s", de->d_name);
        if (id == 0 || !hdr_read(p, h) || sc_rd32(h + 8) != id) {
            ESP_LOGW(TAG, "删除无效录音文件 %s", de->d_name);
            unlink(p);
            continue;
        }
        uint32_t frames = sc_rd32(h + 20);
        if (frames == FRAMES_OPEN) frames = recover_file(id, h);
        if (frames == 0) {
            remove_files(id);
            continue;
        }
        if (s_cat_count >= SC_STORE_MAX_FILES) {
            ESP_LOGW(TAG, "录音条数超过上限 %u,忽略 %lu", SC_STORE_MAX_FILES, (unsigned long)id);
            continue;
        }
        s_cat[s_cat_count++] = (cat_entry_t){
            .file_id = id,
            .start_time = sc_rd32(h + 12),
            .end_time = sc_rd32(h + 16),
            .frames = frames,
        };
    }
    closedir(d);
    cat_sort();
}

esp_err_t sc_store_init(void)
{
    if (s_ready) return ESP_OK;
    if (!s_mutex) s_mutex = xSemaphoreCreateRecursiveMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    esp_vfs_littlefs_conf_t conf = {
        .base_path = BASE_PATH,
        .partition_label = PART_LABEL,
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS 挂载失败: %s", esp_err_to_name(err));
        return err;
    }
    mkdir(REC_DIR, 0775);
    lock();
    scan_catalog();
    s_ready = true;
    unlock();
    uint32_t total, free_kb;
    sc_store_usage(&total, &free_kb);
    ESP_LOGI(TAG, "存储就绪:%u 条录音,%lu/%lu KB 可用", (unsigned)s_cat_count,
             (unsigned long)free_kb, (unsigned long)total);
    return ESP_OK;
}

size_t sc_store_list(sc_file_entry_t *out, size_t cap)
{
    if (!s_ready) return 0;
    lock();
    size_t n = s_cat_count < cap ? s_cat_count : cap;
    for (size_t i = 0; i < n; i++) {
        const cat_entry_t *e = &s_cat[i];
        uint32_t frames = e->frames;
        uint32_t end = e->end_time;
        if (s_live.f && e->file_id == s_live.file_id) {
            frames = s_live.frames;
            end = s_live.start_time + frames / FRAMES_PER_S;
        }
        out[i] = (sc_file_entry_t){ .file_id = e->file_id, .end_time = end, .size = frames * SC_UNIT_DATA };
    }
    unlock();
    return n;
}

size_t sc_store_count(void)
{
    return s_cat_count;
}

bool sc_store_get(uint32_t file_id, sc_store_info_t *out)
{
    if (!s_ready) return false;
    lock();
    bool ok = false;
    if (s_live.f && file_id == s_live.file_id) {
        *out = (sc_store_info_t){
            .file_id = file_id,
            .start_time = s_live.start_time,
            .end_time = s_live.start_time + s_live.frames / FRAMES_PER_S,
            .frames = s_live.frames,
            .live = true,
        };
        memcpy(out->file_key, s_live.file_key, 32);
        memcpy(out->nonce, s_live.nonce, 16);
        ok = true;
    } else if (cat_find(file_id)) {
        char p[48];
        uint8_t h[HDR_SIZE];
        path_dat(p, sizeof(p), file_id);
        if (hdr_read(p, h)) {
            *out = (sc_store_info_t){
                .file_id = file_id,
                .start_time = sc_rd32(h + 12),
                .end_time = sc_rd32(h + 16),
                .frames = sc_rd32(h + 20),
                .live = false,
            };
            memcpy(out->file_key, h + 24, 32);
            memcpy(out->nonce, h + 56, 16);
            ok = out->frames != FRAMES_OPEN;
        }
    }
    unlock();
    return ok;
}

void sc_store_usage(uint32_t *total_kb, uint32_t *free_kb)
{
    size_t total = 0, used = 0;
    if (!s_ready || esp_littlefs_info(PART_LABEL, &total, &used) != ESP_OK) {
        *total_kb = 0;
        *free_kb = 0;
        return;
    }
    *total_kb = (uint32_t)(total / 1024u);
    *free_kb = (uint32_t)((total > used ? total - used : 0) / 1024u);
}

// ---------------------------------------------------------------- 录制

static void live_sync(void)
{
    if (!s_live.f) return;
    fflush(s_live.f);
    fsync(fileno(s_live.f));
    if (s_live.idx) {
        fflush(s_live.idx);
        fsync(fileno(s_live.idx));
    }
    s_live.synced = s_live.frames;
}

void sc_store_sync(void)
{
    lock();
    live_sync();
    unlock();
}

esp_err_t sc_store_begin(uint32_t file_id, uint32_t start_time)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    lock();
    esp_err_t err = ESP_OK;
    if (s_live.f) {
        err = ESP_ERR_INVALID_STATE;
        goto out;
    }
    if (s_cat_count >= SC_STORE_MAX_FILES) {
        err = ESP_ERR_NO_MEM;
        goto out;
    }
    while (cat_find(file_id)) file_id++;      // 同一秒内多次开始:顺延 id

    char p[48];
    memset(&s_live, 0, sizeof(s_live));
    sc_crypto_random(s_live.file_key, sizeof(s_live.file_key));
    sc_crypto_random(s_live.nonce, sizeof(s_live.nonce));
    s_live.file_id = file_id;
    s_live.start_time = start_time;
    s_live.pos = HDR_SIZE;

    path_dat(p, sizeof(p), file_id);
    s_live.f = fopen(p, "w+b");
    path_idx(p, sizeof(p), file_id);
    s_live.idx = fopen(p, "wb");
    if (!s_live.f || !s_live.idx) {
        ESP_LOGE(TAG, "创建录音文件失败: errno=%d", errno);
        if (s_live.f) fclose(s_live.f);
        if (s_live.idx) fclose(s_live.idx);
        memset(&s_live, 0, sizeof(s_live));
        remove_files(file_id);
        err = ESP_FAIL;
        goto out;
    }
    uint8_t h[HDR_SIZE];
    hdr_encode(h, file_id, start_time, start_time, FRAMES_OPEN, s_live.file_key, s_live.nonce);
    fwrite(h, 1, HDR_SIZE, s_live.f);
    live_sync();
    sc_ring_reset(&s_ring, 0);

    s_cat[s_cat_count++] = (cat_entry_t){ .file_id = file_id, .start_time = start_time,
                                          .end_time = start_time, .frames = 0 };
    cat_sort();
    ESP_LOGI(TAG, "开始录音文件 %lu", (unsigned long)file_id);
out:
    unlock();
    return err;
}

esp_err_t sc_store_append(const uint8_t *frame, uint8_t len)
{
    if (len > SC_UNIT_DATA) return ESP_ERR_INVALID_ARG;
    lock();
    esp_err_t err = ESP_OK;
    if (!s_live.f) {
        err = ESP_ERR_INVALID_STATE;
        goto out;
    }
    // 环形缓冲即将淘汰尚未同步的帧时先同步,保证"环外的帧一定能从文件读到"。
    if (sc_ring_first_after_push(&s_ring, len) > s_live.synced) live_sync();

    if (s_live.frames % SC_STORE_IDX_STRIDE == 0) {
        uint8_t b[4];
        sc_wr32(b, s_live.pos);
        fwrite(b, 1, 4, s_live.idx);
    }
    if (fputc(len, s_live.f) == EOF || (len && fwrite(frame, 1, len, s_live.f) != len)) {
        err = ESP_FAIL;
        goto out;
    }
    s_live.pos += 1u + len;
    s_live.frames++;
    sc_ring_push(&s_ring, frame, len);
    if (s_live.frames - s_live.synced >= SYNC_EVERY) live_sync();
out:
    unlock();
    return err;
}

esp_err_t sc_store_end(uint32_t end_time)
{
    lock();
    if (!s_live.f) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t id = s_live.file_id;
    uint32_t frames = s_live.frames;
    uint8_t h[HDR_SIZE];
    hdr_encode(h, id, s_live.start_time, end_time, frames, s_live.file_key, s_live.nonce);
    fflush(s_live.f);
    fseek(s_live.f, 0, SEEK_SET);
    fwrite(h, 1, HDR_SIZE, s_live.f);
    fclose(s_live.f);
    fclose(s_live.idx);
    memset(&s_live, 0, sizeof(s_live));

    // 读者若正读这条录音,下一次读取会按文件重新打开(帧数已固定)。
    for (int i = 0; i < SC_STORE_READERS; i++) {
        if (s_readers[i].used && s_readers[i].file_id == id && s_readers[i].f) {
            fclose(s_readers[i].f);
            s_readers[i].f = NULL;
        }
    }
    cat_entry_t *e = cat_find(id);
    if (frames == 0) {
        cat_remove(id);
        remove_files(id);
    } else if (e) {
        e->frames = frames;
        e->end_time = end_time;
    }
    ESP_LOGI(TAG, "录音文件 %lu 结束:%lu 帧", (unsigned long)id, (unsigned long)frames);
    unlock();
    return ESP_OK;
}

uint32_t sc_store_live_id(void)
{
    return s_live.f ? s_live.file_id : 0;
}

uint32_t sc_store_live_frames(void)
{
    return s_live.f ? s_live.frames : 0;
}

// ---------------------------------------------------------------- 读取

sc_reader_t *sc_store_reader_open(uint32_t file_id)
{
    if (!s_ready) return NULL;
    lock();
    sc_reader_t *rd = NULL;
    if (cat_find(file_id)) {
        for (int i = 0; i < SC_STORE_READERS; i++) {
            if (!s_readers[i].used) {
                rd = &s_readers[i];
                *rd = (sc_reader_t){ .used = true, .file_id = file_id };
                break;
            }
        }
    }
    unlock();
    return rd;
}

void sc_store_reader_close(sc_reader_t *rd)
{
    if (!rd) return;
    lock();
    if (rd->f) fclose(rd->f);
    *rd = (sc_reader_t){ 0 };
    unlock();
}

static uint32_t idx_lookup(uint32_t id, uint32_t block)
{
    char p[48];
    path_idx(p, sizeof(p), id);
    FILE *f = fopen(p, "rb");
    uint32_t pos = HDR_SIZE;
    if (f) {
        uint8_t b[4];
        if (fseek(f, (long)block * 4, SEEK_SET) == 0 && fread(b, 1, 4, f) == 4) pos = sc_rd32(b);
        fclose(f);
    }
    return pos;
}

static bool reader_skip_to(sc_reader_t *rd, uint32_t seq)
{
    while (rd->cur_seq < seq) {
        int len = fgetc(rd->f);
        if (len == EOF || len > (int)SC_UNIT_DATA) return false;
        if (len > 0 && fseek(rd->f, len, SEEK_CUR) != 0) return false;
        rd->cur_seq++;
    }
    return true;
}

int sc_store_reader_read(sc_reader_t *rd, uint32_t seq, uint8_t (*bufs)[SC_UNIT_DATA],
                         uint8_t *lens, int max)
{
    if (!rd || !rd->used || max <= 0) return -1;
    lock();
    int n = 0;
    bool live = s_live.f && s_live.file_id == rd->file_id;
    uint32_t total;
    if (live) {
        total = s_live.frames;
    } else {
        cat_entry_t *e = cat_find(rd->file_id);
        if (!e) { n = -1; goto out; }
        total = e->frames;
    }
    if (seq >= total) goto out;

    if (live && sc_ring_contains(&s_ring, seq)) {
        while (n < max && sc_ring_get(&s_ring, seq + (uint32_t)n, bufs[n], &lens[n])) n++;
        goto out;
    }

    // 文件路径:录制中的文件只能读到已同步部分;不够就重新打开以看到新数据。
    uint32_t limit = live ? s_live.synced : total;
    if (!rd->f || seq >= rd->visible) {
        if (rd->f) fclose(rd->f);
        char p[48];
        path_dat(p, sizeof(p), rd->file_id);
        rd->f = fopen(p, "rb");
        rd->visible = limit;
        rd->cur_seq = UINT32_MAX;
        if (!rd->f) { n = -1; goto out; }
    }
    if (rd->cur_seq == UINT32_MAX || rd->cur_seq > seq || seq - rd->cur_seq > SC_STORE_IDX_STRIDE) {
        // 新句柄、向后跳或跳得远:按索引重新定位,再顺序跳过块内剩余记录。
        uint32_t block = seq / SC_STORE_IDX_STRIDE;
        uint32_t pos = idx_lookup(rd->file_id, block);
        rd->cur_seq = pos == HDR_SIZE ? 0 : block * SC_STORE_IDX_STRIDE;
        if (fseek(rd->f, (long)pos, SEEK_SET) != 0) { n = -1; goto out; }
    }
    if (!reader_skip_to(rd, seq)) { n = -1; goto out; }
    uint32_t end = rd->visible < total ? rd->visible : total;
    while (n < max && rd->cur_seq < end) {
        int len = fgetc(rd->f);
        if (len == EOF || len > (int)SC_UNIT_DATA) { if (n == 0) n = -1; break; }
        if (len > 0 && fread(bufs[n], 1, (size_t)len, rd->f) != (size_t)len) { if (n == 0) n = -1; break; }
        lens[n] = (uint8_t)len;
        rd->cur_seq++;
        n++;
    }
out:
    unlock();
    return n;
}

// ---------------------------------------------------------------- 管理

esp_err_t sc_store_delete(uint32_t file_id)
{
    lock();
    esp_err_t err = ESP_OK;
    if (s_live.f && s_live.file_id == file_id) {
        err = ESP_ERR_INVALID_STATE;
    } else if (!cat_find(file_id)) {
        err = ESP_ERR_NOT_FOUND;
    } else {
        for (int i = 0; i < SC_STORE_READERS; i++) {
            if (s_readers[i].used && s_readers[i].file_id == file_id && s_readers[i].f) {
                fclose(s_readers[i].f);
                s_readers[i].f = NULL;
            }
        }
        remove_files(file_id);
        cat_remove(file_id);
        ESP_LOGI(TAG, "删除录音 %lu", (unsigned long)file_id);
    }
    unlock();
    return err;
}

esp_err_t sc_store_wipe(void)
{
    lock();
    if (s_live.f) sc_store_end(s_live.start_time);
    for (int i = 0; i < SC_STORE_READERS; i++) {
        if (s_readers[i].f) fclose(s_readers[i].f);
        s_readers[i] = (sc_reader_t){ 0 };
    }
    while (s_cat_count > 0) {
        remove_files(s_cat[s_cat_count - 1].file_id);
        s_cat_count--;
    }
    unlock();
    ESP_LOGW(TAG, "已清空全部录音");
    return ESP_OK;
}
