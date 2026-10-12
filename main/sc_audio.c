// main/sc_audio.c —— 麦克风采集 + Opus 编码任务。
#include "sc_audio.h"

#include "bsp_audio.h"
#include "sc_proto.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "opus.h"

static const char *TAG = "sc_audio";

// SILK 定点编码栈占用大;参考固件(同芯片 Pocket Intercom,60 ms 帧)用 24 KB。
// libopus 以 USE_ALLOCA 编译,栈用量随信号变化:实测峰值约 21 KB,这里给 24 KB,
// 并在每次停止时打印剩余栈。
#define AUDIO_TASK_STACK  (24 * 1024)
#define AUDIO_TASK_PRIO   6

// libopus 内部请求(opus_private.h):强制编码模式。值在 libopus 中长期稳定。
#define SC_OPUS_SET_FORCE_MODE_REQUEST 11002
#define SC_OPUS_MODE_SILK_ONLY         1000

static OpusEncoder *s_enc;
static sc_audio_frame_cb_t s_cb;
static void *s_cb_ctx;
static TaskHandle_t s_task;
static StaticTask_t s_task_tcb;
static StackType_t s_task_stack[AUDIO_TASK_STACK / sizeof(StackType_t)];
static SemaphoreHandle_t s_stopped;
static volatile bool s_run;
static volatile bool s_running;
static volatile uint8_t s_level;
static int16_t s_pcm[SC_AUDIO_FRAME_SAMPLES];
static uint8_t s_pkt[SC_UNIT_DATA];

static uint8_t level_of(const int16_t *pcm, size_t n)
{
    int peak = 0;
    for (size_t i = 0; i < n; i++) {
        int v = pcm[i] < 0 ? -pcm[i] : pcm[i];
        if (v > peak) peak = v;
    }
    // 粗略对数刻度:约 -48 dBFS → 0,0 dBFS → 100。
    int level = 0;
    while (peak > 128 && level < 100) {
        peak >>= 1;
        level += 12;
    }
    return (uint8_t)(level > 100 ? 100 : level);
}

static void capture_loop(void)
{
    // 只录音不放音:输出音量置 0,避免 codec 输出通路的底噪(BSP 以全双工打开 codec)。
    bsp_audio_set_volume(0);
    if (bsp_audio_wake() != ESP_OK || bsp_audio_set_format(SC_AUDIO_RATE, 16, 1) != ESP_OK) {
        ESP_LOGE(TAG, "codec 打开失败");
        return;
    }
    opus_encoder_ctl(s_enc, OPUS_RESET_STATE);
    s_running = true;
    uint32_t frames = 0, bytes = 0;
    while (s_run) {
        if (bsp_audio_read(s_pcm, sizeof(s_pcm)) != ESP_OK) {
            ESP_LOGW(TAG, "bsp_audio_read 失败,丢一帧");
            continue;
        }
        s_level = level_of(s_pcm, SC_AUDIO_FRAME_SAMPLES);
        int n = opus_encode(s_enc, s_pcm, SC_AUDIO_FRAME_SAMPLES, s_pkt, sizeof(s_pkt));
        if (n < 0) {
            ESP_LOGW(TAG, "opus_encode 失败: %d", n);
            continue;
        }
        frames++;
        bytes += (uint32_t)n;
        if (s_cb) s_cb(s_pkt, (uint8_t)n, s_cb_ctx);
    }
    s_running = false;
    s_level = 0;
    if (frames) {
        ESP_LOGI(TAG, "采集结束:%lu 帧,平均 %lu B/帧,栈剩余 %u B", (unsigned long)frames,
                 (unsigned long)(bytes / frames), (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
}

static void audio_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_run) capture_loop();
        if (bsp_audio_sleep() != ESP_OK) ESP_LOGW(TAG, "codec 休眠失败");
        xSemaphoreGive(s_stopped);
    }
}

esp_err_t sc_audio_init(sc_audio_frame_cb_t cb, void *ctx)
{
    if (s_task) return ESP_OK;
    s_cb = cb;
    s_cb_ctx = ctx;

    esp_err_t err = bsp_audio_init();
    if (err != ESP_OK) return err;
    (void)bsp_audio_sleep();                    // 空闲时 codec 保持休眠

    int size = opus_encoder_get_size(1);
    s_enc = heap_caps_malloc((size_t)size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_enc) return ESP_ERR_NO_MEM;
    int rc = opus_encoder_init(s_enc, SC_AUDIO_RATE, 1, OPUS_APPLICATION_VOIP);
    if (rc != OPUS_OK) {
        ESP_LOGE(TAG, "opus_encoder_init 失败: %d", rc);
        free(s_enc);
        s_enc = NULL;
        return ESP_FAIL;
    }
    opus_encoder_ctl(s_enc, OPUS_SET_BITRATE(SC_AUDIO_BITRATE));
    opus_encoder_ctl(s_enc, OPUS_SET_COMPLEXITY(0));
    opus_encoder_ctl(s_enc, OPUS_SET_VBR(1));
    opus_encoder_ctl(s_enc, OPUS_SET_DTX(0));
    opus_encoder_ctl(s_enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(s_enc, OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_WIDEBAND));
    opus_encoder_ctl(s_enc, SC_OPUS_SET_FORCE_MODE_REQUEST, SC_OPUS_MODE_SILK_ONLY);

    s_stopped = xSemaphoreCreateBinary();
    if (!s_stopped) return ESP_ERR_NO_MEM;
    s_task = xTaskCreateStatic(audio_task, "sc_audio", AUDIO_TASK_STACK / sizeof(StackType_t), NULL,
                               AUDIO_TASK_PRIO, s_task_stack, &s_task_tcb);
    ESP_LOGI(TAG, "Opus 编码器就绪(%d B 状态,%d bps SILK-WB)", size, SC_AUDIO_BITRATE);
    return s_task ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t sc_audio_start(void)
{
    if (!s_task) return ESP_ERR_INVALID_STATE;
    if (s_run) return ESP_OK;
    s_run = true;
    xSemaphoreTake(s_stopped, 0);               // 清掉残留信号
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

void sc_audio_stop(void)
{
    if (!s_task || !s_run) return;
    s_run = false;
    // 采集循环最多再阻塞一个 I2S 读周期(20 ms)。
    if (xSemaphoreTake(s_stopped, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGW(TAG, "等待采集任务停止超时");
    }
}

bool sc_audio_running(void)
{
    return s_running;
}

uint8_t sc_audio_level(void)
{
    return s_level;
}
