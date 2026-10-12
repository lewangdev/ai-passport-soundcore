// main/sc_audio.h —— 麦克风采集 + Opus 编码(16 kHz 单声道,20 ms/帧)。
//
// D3200 的线上音频是裸 Opus 帧序列,每帧装进 160 字节单元、尾部补零;客户端解码前
// 会剥掉尾部的 0。只有 SILK(纯 LPC)模式下剥掉尾零是无损的(范围解码器把越界字节
// 视为 0,libopus 编码器在该模式下本身也会裁掉尾零),所以编码器被固定在 SILK 宽带模式。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define SC_AUDIO_RATE        16000
#define SC_AUDIO_FRAME_MS    20
#define SC_AUDIO_FRAME_SAMPLES (SC_AUDIO_RATE * SC_AUDIO_FRAME_MS / 1000)
#define SC_AUDIO_BITRATE     16000

// 在编码任务中调用;须快速返回(写存储即可)。
typedef void (*sc_audio_frame_cb_t)(const uint8_t *frame, uint8_t len, void *ctx);

// 创建编码器与常驻采集任务(静态栈)。应在 BLE/Wi-Fi 占用堆之前调用。
esp_err_t sc_audio_init(sc_audio_frame_cb_t cb, void *ctx);
// 开始/停止采集。stop 阻塞到采集循环退出、codec 进入休眠。
esp_err_t sc_audio_start(void);
void sc_audio_stop(void);
bool sc_audio_running(void);
// 最近一帧的电平 0..100(界面用)。
uint8_t sc_audio_level(void);
