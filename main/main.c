// main/main.c —— AI Passport 模拟 soundcore Work(安克 AI 录音豆 D3200)。
//
// 手机端(Soundcore Manager / 飞书)把本机当成一颗 D3200:BLE 扫描连接、读设备信息、
// 同步时间、ECDH 加密握手、开始/暂停录音、实时转写音频流、离线文件拉取、
// Wi-Fi SoftAP 批量导出、绑定/解绑、恢复出厂。协议实现见 sc_core.c。
//
// 按键:
//   OK   单击  开始 / 暂停 / 继续录音
//   OK   长按  结束并保存录音;Wi-Fi 导出中则关闭热点
//   UP / DOWN 单击  切换页面(录音 / 录音列表 / 设备信息)
// 一分钟无操作屏幕调暗,任意键唤醒(唤醒那一下不触发动作)。
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "key_guard.h"
#include "sc_core.h"
#include "sc_store.h"
#include "sc_ui.h"
#include "sc_wifi.h"

#include "bootloader_random.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "main";

#define INPUT_QUEUE_DEPTH   8
#define BACKLIGHT_ON        100
#define BACKLIGHT_DIM       8
#define BACKLIGHT_IDLE_US   (60LL * 1000 * 1000)
#define KEY_GUARD_MAX_US    (10LL * 1000 * 1000)

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

static QueueHandle_t s_input_queue;
static volatile bool s_input_ready;
static key_guard_t s_key_guard;
static int64_t s_last_input_us;
static bool s_dimmed;
static bool s_swallow_press;    // 唤醒屏幕的那次按键:吞掉直到松手

static void backlight_wake(void)
{
    s_last_input_us = esp_timer_get_time();
    if (s_dimmed) {
        s_dimmed = false;
        bsp_display_backlight(BACKLIGHT_ON);
    }
}

static void process_input(const input_event_t *in)
{
    if (s_swallow_press) {
        if (in->event == BSP_BTN_RELEASE) s_swallow_press = false;
        return;
    }
    if (s_dimmed) {
        backlight_wake();
        s_swallow_press = in->event != BSP_BTN_RELEASE;
        return;
    }
    s_last_input_us = esp_timer_get_time();
    if (in->event == BSP_BTN_CLICK && in->btn == BSP_BTN_UP) {
        sc_ui_page_step(-1);
    } else if (in->event == BSP_BTN_CLICK && in->btn == BSP_BTN_DOWN) {
        sc_ui_page_step(1);
    } else if (in->btn == BSP_BTN_OK && (in->event == BSP_BTN_CLICK || in->event == BSP_BTN_LONG)) {
        sc_core_key(in->btn, in->event);
    }
}

static void input_task(void *arg)
{
    (void)arg;
    input_event_t in;
    for (;;) {
        if (xQueueReceive(s_input_queue, &in, pdMS_TO_TICKS(1000)) == pdTRUE) {
            process_input(&in);
        } else if (!s_dimmed && !sc_ui_wifi_page() &&
                   esp_timer_get_time() - s_last_input_us > BACKLIGHT_IDLE_US) {
            s_dimmed = true;
            bsp_display_backlight(BACKLIGHT_DIM);
        }
    }
}

// 按键回调运行在 button 组件的共享 esp_timer 任务:只入队,立即返回。
// 守卫必须先于就绪检查消费事件(见 key_guard.h 与 tests/test_key_wake_contract.py)。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    if (key_guard_consume(&s_key_guard, ev == BSP_BTN_RELEASE, esp_timer_get_time())) return;
    if (!s_input_ready || !s_input_queue) return;
    const input_event_t in = { .btn = btn, .event = ev };
    (void)xQueueSend(s_input_queue, &in, 0);
}

void app_main(void)
{
    ESP_LOGI(TAG, "AI Passport · soundcore Work (D3200) 模拟器启动");
    if (esp_sleep_get_wakeup_causes() & (1u << ESP_SLEEP_WAKEUP_GPIO)) {
        key_guard_arm(&s_key_guard, esp_timer_get_time(), KEY_GUARD_MAX_US);
    }

    // BLE/PHY 校准与本应用设置都在 NVS;失败时不擦除用户数据。
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) ESP_LOGE(TAG, "NVS 初始化失败: %s(未自动擦除)", esp_err_to_name(err));

    bsp_i2c_init();
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(BACKLIGHT_ON);
    if (bsp_battery_init() != ESP_OK) ESP_LOGW(TAG, "电量计不可用");

    if (sc_store_init() != ESP_OK) ESP_LOGE(TAG, "录音存储不可用");
    ESP_LOGI(TAG, "启动 BLE 前空闲堆 %u B", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    // 首次使用时生成 Wi-Fi 导出用的自签名证书。放在 BLE 启动前:此时堆充裕;
    // RF 尚未开启,临时打开 SAR ADC 熵源保证 esp_fill_random 为真随机。
    bootloader_random_enable();
    if (sc_wifi_prepare_cert() != ESP_OK) ESP_LOGW(TAG, "TLS 证书准备失败,导出时重试");
    bootloader_random_disable();
    if (sc_core_start() != ESP_OK) {
        ESP_LOGE(TAG, "核心任务启动失败");
        return;
    }

    if (bsp_lvgl_lock(1000)) {
        sc_ui_init();
        bsp_lvgl_unlock();
    }

    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    if (s_input_queue && xTaskCreate(input_task, "sc_input", 3072, NULL, 5, NULL) == pdPASS) {
        s_last_input_us = esp_timer_get_time();
        if (bsp_button_init(on_key, NULL) == ESP_OK) {
            if (s_key_guard.armed) {
                key_guard_sync_released(&s_key_guard, bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN);
            }
            s_input_ready = true;
        } else {
            ESP_LOGE(TAG, "按键初始化失败");
        }
    }

    ESP_LOGI(TAG, "就绪,空闲堆 %u B(最大块 %u B)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}
