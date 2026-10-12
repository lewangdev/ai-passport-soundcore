// main/sc_wifi.c —— SoftAP + wss:// WebSocket 服务实现。
#include "sc_wifi.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_https_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "mbedtls/ecp.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include "nvs.h"
#include "sc_crypto.h"

static const char *TAG = "sc_wifi";

#define NVS_NS          "sc_tls"
#define CERT_PEM_MAX    1024
#define KEY_PEM_MAX     512
#define WS_RX_MAX       1024

static sc_wifi_cbs_t s_cbs;
static httpd_handle_t s_httpd;
static esp_netif_t *s_netif;
static esp_event_handler_instance_t s_wifi_handler;
static bool s_wifi_inited;
static bool s_running;
static bool s_netif_ready;
static char *s_cert;
static char *s_key;
static sc_rx_t s_rx;

static void emit(sc_wifi_event_t ev)
{
    if (s_cbs.on_event) s_cbs.on_event(ev);
}

// ---------------------------------------------------------------- TLS 证书

static int rng_cb(void *ctx, unsigned char *out, size_t len)
{
    (void)ctx;
    sc_crypto_random(out, len);
    return 0;
}

static esp_err_t generate_cert(char *cert, char *key)
{
    mbedtls_pk_context pk;
    mbedtls_x509write_cert crt;
    mbedtls_pk_init(&pk);
    mbedtls_x509write_crt_init(&crt);
    static const unsigned char serial[] = { 0x01 };

    int rc = mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (rc == 0) rc = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(pk), rng_cb, NULL);
    if (rc == 0) {
        mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
        mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
        mbedtls_x509write_crt_set_subject_key(&crt, &pk);
        mbedtls_x509write_crt_set_issuer_key(&crt, &pk);
        rc = mbedtls_x509write_crt_set_subject_name(&crt, "CN=soundcore-work,O=AI Passport");
    }
    if (rc == 0) rc = mbedtls_x509write_crt_set_issuer_name(&crt, "CN=soundcore-work,O=AI Passport");
    if (rc == 0) rc = mbedtls_x509write_crt_set_serial_raw(&crt, (unsigned char *)serial, sizeof(serial));
    if (rc == 0) rc = mbedtls_x509write_crt_set_validity(&crt, "20250101000000", "20491231235959");
    if (rc == 0) rc = mbedtls_x509write_crt_pem(&crt, (unsigned char *)cert, CERT_PEM_MAX, rng_cb, NULL);
    if (rc == 0) rc = mbedtls_pk_write_key_pem(&pk, (unsigned char *)key, KEY_PEM_MAX);

    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&pk);
    if (rc != 0) {
        ESP_LOGE(TAG, "生成 TLS 证书失败: -0x%04x", -rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t sc_wifi_prepare_cert(void)
{
    if (s_cert && s_key) return ESP_OK;
    if (!s_cert) s_cert = calloc(1, CERT_PEM_MAX);
    if (!s_key) s_key = calloc(1, KEY_PEM_MAX);
    if (!s_cert || !s_key) return ESP_ERR_NO_MEM;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    size_t clen = CERT_PEM_MAX, klen = KEY_PEM_MAX;
    if (nvs_get_str(h, "cert", s_cert, &clen) == ESP_OK && nvs_get_str(h, "key", s_key, &klen) == ESP_OK) {
        nvs_close(h);
        return ESP_OK;
    }
    ESP_LOGI(TAG, "首次使用:生成自签名 TLS 证书");
    err = generate_cert(s_cert, s_key);
    if (err == ESP_OK) err = nvs_set_str(h, "cert", s_cert);
    if (err == ESP_OK) err = nvs_set_str(h, "key", s_key);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// ---------------------------------------------------------------- WebSocket

typedef struct {
    sc_wifi_sink_t base;
    httpd_req_t *req;
} ws_sink_t;

static esp_err_t ws_sink_send(sc_wifi_sink_t *sink, const uint8_t *data, size_t len)
{
    ws_sink_t *ws = (ws_sink_t *)sink;
    httpd_ws_frame_t f = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_BINARY,
        .payload = (uint8_t *)data,
        .len = len,
    };
    return httpd_ws_send_frame(ws->req, &f);
}

static esp_err_t ws_send_text(httpd_req_t *req, const char *text)
{
    httpd_ws_frame_t f = {
        .final = true,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)text,
        .len = strlen(text),
    };
    return httpd_ws_send_frame(req, &f);
}

static void ws_on_cmd(const sc_cmd_t *cmd, void *ctx)
{
    ws_sink_t *sink = ctx;
    if (cmd->type == SC_T_FILE && cmd->id == SC_ID_FILE_DONE) {
        emit(SC_WIFI_EV_FINISH);
        return;
    }
    if (s_cbs.on_command) s_cbs.on_command(cmd, &sink->base);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WebSocket 已连接,空闲堆 %u B,历史最低 %u B",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
        sc_rx_init(&s_rx, false);
        emit(SC_WIFI_EV_WS_OPEN);
        return ESP_OK;
    }
    httpd_ws_frame_t f = { 0 };
    esp_err_t err = httpd_ws_recv_frame(req, &f, 0);
    if (err != ESP_OK) return err;
    if (f.len > WS_RX_MAX) return ESP_ERR_INVALID_SIZE;
    uint8_t *buf = calloc(1, f.len + 1);
    if (!buf) return ESP_ERR_NO_MEM;
    f.payload = buf;
    err = f.len ? httpd_ws_recv_frame(req, &f, f.len) : ESP_OK;
    if (err != ESP_OK) {
        free(buf);
        return err;
    }
    emit(SC_WIFI_EV_ACTIVITY);

    ws_sink_t sink = { .base = { .send = ws_sink_send }, .req = req };
    if (f.type == HTTPD_WS_TYPE_TEXT) {
        const char *t = (const char *)buf;
        if (strcasecmp(t, "ping") == 0) {
            ws_send_text(req, "pong");
        } else if (strcasecmp(t, "FINISH") == 0) {
            ESP_LOGI(TAG, "客户端结束会话");
            ws_send_text(req, "OK");
            emit(SC_WIFI_EV_FINISH);
        } else {
            uint8_t bin[256];
            size_t n = sc_hex_decode(t, f.len, bin, sizeof(bin));
            if (n) sc_rx_feed(&s_rx, bin, n, ws_on_cmd, &sink);
            else ESP_LOGW(TAG, "未识别的文本帧: %.32s", t);
        }
    } else if (f.type == HTTPD_WS_TYPE_BINARY) {
        sc_rx_feed(&s_rx, buf, f.len, ws_on_cmd, &sink);
    }
    free(buf);
    return ESP_OK;
}

static void on_session_close(httpd_handle_t hd, int sockfd)
{
    (void)hd;
    ESP_LOGI(TAG, "连接关闭 fd=%d", sockfd);
    close(sockfd);
    emit(SC_WIFI_EV_WS_CLOSED);
}

static esp_err_t start_server(void)
{
    httpd_ssl_config_t conf = HTTPD_SSL_CONFIG_DEFAULT();
    conf.servercert = (const uint8_t *)s_cert;
    conf.servercert_len = strlen(s_cert) + 1;
    conf.prvtkey_pem = (const uint8_t *)s_key;
    conf.prvtkey_len = strlen(s_key) + 1;
    conf.port_secure = SC_WIFI_PORT;
    conf.httpd.max_open_sockets = 2;
    conf.httpd.lru_purge_enable = true;
    conf.httpd.close_fn = on_session_close;
    conf.httpd.stack_size = 10 * 1024;
    esp_err_t err = httpd_ssl_start(&s_httpd, &conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTPS 服务启动失败: %s", esp_err_to_name(err));
        return err;
    }
    static const httpd_uri_t ws = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = ws_handler,
        .is_websocket = true,
        .handle_ws_control_frames = false,
    };
    return httpd_register_uri_handler(s_httpd, &ws);
}

// ---------------------------------------------------------------- SoftAP

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "手机已加入热点");
        emit(SC_WIFI_EV_STA_JOINED);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        ESP_LOGI(TAG, "手机离开热点");
        emit(SC_WIFI_EV_STA_LEFT);
    }
}

esp_err_t sc_wifi_start(const char *ssid, const char *password, const sc_wifi_cbs_t *cbs)
{
    if (s_running) return ESP_ERR_INVALID_STATE;
    s_cbs = *cbs;
    esp_err_t err = sc_wifi_prepare_cert();
    if (err != ESP_OK) return err;

    if (!s_netif_ready) {
        err = esp_netif_init();
        if (err == ESP_OK) {
            err = esp_event_loop_create_default();
            if (err == ESP_ERR_INVALID_STATE) err = ESP_OK;
        }
        if (err != ESP_OK) return err;
        s_netif_ready = true;
    }
    s_netif = esp_netif_create_default_wifi_ap();
    if (!s_netif) return ESP_ERR_NO_MEM;

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init);
    if (err != ESP_OK) goto fail;
    s_wifi_inited = true;
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, &s_wifi_handler);
    if (err != ESP_OK) goto fail;

    wifi_config_t ap = { 0 };
    size_t slen = strnlen(ssid, sizeof(ap.ap.ssid));
    memcpy(ap.ap.ssid, ssid, slen);
    ap.ap.ssid_len = (uint8_t)slen;
    strlcpy((char *)ap.ap.password, password, sizeof(ap.ap.password));
    ap.ap.channel = 6;
    ap.ap.max_connection = 2;
    ap.ap.authmode = password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ap.ap.pmf_cfg.required = false;

    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err != ESP_OK) goto fail;
    err = start_server();
    if (err != ESP_OK) goto fail;
    s_running = true;
    ESP_LOGI(TAG, "SoftAP「%s」已开启,wss://192.168.4.1:%d,空闲堆 %u B(最大块 %u B)", ssid, SC_WIFI_PORT,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "Wi-Fi 启动失败: %s", esp_err_to_name(err));
    s_running = true;                           // 让 stop 统一回滚
    sc_wifi_stop();
    return err;
}

void sc_wifi_stop(void)
{
    if (!s_running) return;
    if (s_httpd) {
        httpd_ssl_stop(s_httpd);
        s_httpd = NULL;
    }
    if (s_wifi_handler) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_handler);
        s_wifi_handler = NULL;
    }
    if (s_wifi_inited) {
        esp_wifi_stop();
        esp_wifi_deinit();
        s_wifi_inited = false;
    }
    if (s_netif) {
        esp_netif_destroy_default_wifi(s_netif);
        s_netif = NULL;
    }
    s_running = false;
    ESP_LOGI(TAG, "SoftAP 已关闭");
}

bool sc_wifi_running(void)
{
    return s_running;
}

void sc_wifi_ip(uint8_t ip[4])
{
    // esp_netif 默认 SoftAP 地址。
    ip[0] = 192;
    ip[1] = 168;
    ip[2] = 4;
    ip[3] = 1;
}
