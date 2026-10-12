// main/sc_ui.c —— 模拟器界面(LVGL 9,240x320 竖屏,四角 30 px 圆角遮罩)。
#include "sc_ui.h"

#include "sc_core.h"
#include "sc_proto.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "lvgl.h"

#define COL_BG      0x0D1015
#define COL_CARD    0x1A1F27
#define COL_TEXT    0xE8ECF1
#define COL_DIM     0x7D8794
#define COL_ACCENT  0x2F80FF
#define COL_REC     0xFF4D4F
#define COL_PAUSE   0xFFB020
#define COL_OK      0x3DD68C

#define PAGE_COUNT  3
#define FILE_ROWS   8

enum { PAGE_RECORDER = 0, PAGE_FILES, PAGE_DEVICE };

static atomic_int s_page_req;
static atomic_bool s_wifi_shown;
static int s_page = -1;

static lv_obj_t *s_scr;
static lv_obj_t *s_link_lbl;
static lv_obj_t *s_batt_lbl;
static lv_obj_t *s_pages[PAGE_COUNT];
static lv_obj_t *s_wifi_page;
static lv_obj_t *s_dots[PAGE_COUNT];

// 录音页
static lv_obj_t *s_ring;
static lv_obj_t *s_state_lbl;
static lv_obj_t *s_time_lbl;
static lv_obj_t *s_file_lbl;
static lv_obj_t *s_chip_bound, *s_chip_secure, *s_chip_live;
static lv_obj_t *s_store_lbl;
static lv_obj_t *s_store_bar;
static lv_obj_t *s_hint_lbl;

// 列表页 / 设备页 / Wi-Fi 页
static lv_obj_t *s_file_rows[FILE_ROWS];
static lv_obj_t *s_files_empty;
static lv_obj_t *s_dev_lbl;
static lv_obj_t *s_wifi_ssid, *s_wifi_pwd, *s_wifi_state, *s_wifi_count;

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, "");
    return l;
}

static lv_obj_t *page_create(lv_obj_t *parent)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, 240, 238);
    lv_obj_set_pos(p, 0, 40);
    lv_obj_set_hidden(p, true);
    lv_obj_set_scrollable(p, false);
    return p;
}

static lv_obj_t *chip(lv_obj_t *parent, const char *text, int x)
{
    lv_obj_t *c = lv_label_create(parent);
    lv_label_set_text(c, text);
    lv_obj_set_style_text_font(c, &lv_font_montserrat_14, 0);
    lv_obj_set_style_radius(c, 10, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(c, 8, 0);
    lv_obj_set_style_pad_ver(c, 2, 0);
    lv_obj_set_pos(c, x, 184);
    return c;
}

static void chip_set(lv_obj_t *c, bool on, uint32_t color)
{
    lv_obj_set_style_bg_color(c, lv_color_hex(on ? color : COL_CARD), 0);
    lv_obj_set_style_text_color(c, lv_color_hex(on ? COL_BG : COL_DIM), 0);
}

static void build_recorder(lv_obj_t *p)
{
    lv_obj_t *title = label(p, &lv_font_montserrat_14, COL_DIM);
    lv_label_set_text(title, "soundcore Work  -  D3200");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 2);

    s_ring = lv_arc_create(p);
    lv_obj_set_size(s_ring, 150, 150);
    lv_obj_align(s_ring, LV_ALIGN_TOP_MID, 0, 26);
    lv_arc_set_bg_angles(s_ring, 0, 360);
    lv_arc_set_rotation(s_ring, 270);
    lv_arc_set_range(s_ring, 0, 100);
    lv_arc_set_value(s_ring, 0);
    lv_obj_remove_style(s_ring, NULL, LV_PART_KNOB);
    lv_obj_set_clickable(s_ring, false);
    lv_obj_set_style_arc_width(s_ring, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_ring, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_ring, lv_color_hex(COL_CARD), LV_PART_MAIN);

    s_state_lbl = label(p, &lv_font_montserrat_14, COL_DIM);
    lv_obj_align(s_state_lbl, LV_ALIGN_TOP_MID, 0, 66);
    s_time_lbl = label(p, &lv_font_montserrat_32, COL_TEXT);
    lv_obj_align(s_time_lbl, LV_ALIGN_TOP_MID, 0, 86);
    s_file_lbl = label(p, &lv_font_montserrat_14, COL_DIM);
    lv_obj_align(s_file_lbl, LV_ALIGN_TOP_MID, 0, 126);

    s_chip_bound = chip(p, "BOUND", 22);
    s_chip_secure = chip(p, "SECURE", 90);
    s_chip_live = chip(p, "LIVE", 166);

    s_store_lbl = label(p, &lv_font_montserrat_14, COL_DIM);
    lv_obj_align(s_store_lbl, LV_ALIGN_TOP_MID, 0, 210);
    s_store_bar = lv_bar_create(p);
    lv_obj_set_size(s_store_bar, 180, 4);
    lv_obj_align(s_store_bar, LV_ALIGN_TOP_MID, 0, 231);
    lv_bar_set_range(s_store_bar, 0, 1000);
    lv_obj_set_style_bg_color(s_store_bar, lv_color_hex(COL_CARD), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_store_bar, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);
}

static void build_files(lv_obj_t *p)
{
    lv_obj_t *title = label(p, &lv_font_montserrat_20, COL_TEXT);
    lv_label_set_text(title, "Recordings");
    lv_obj_set_pos(title, 22, 4);
    for (int i = 0; i < FILE_ROWS; i++) {
        s_file_rows[i] = label(p, &lv_font_montserrat_14, COL_TEXT);
        lv_obj_set_pos(s_file_rows[i], 22, 40 + i * 26);
    }
    s_files_empty = label(p, &lv_font_montserrat_14, COL_DIM);
    lv_label_set_text(s_files_empty, "No recordings yet.\nPress OK to record.");
    lv_obj_set_pos(s_files_empty, 22, 44);
}

static void build_device(lv_obj_t *p)
{
    lv_obj_t *title = label(p, &lv_font_montserrat_20, COL_TEXT);
    lv_label_set_text(title, "Device");
    lv_obj_set_pos(title, 22, 4);
    s_dev_lbl = label(p, &lv_font_montserrat_14, COL_TEXT);
    lv_obj_set_pos(s_dev_lbl, 22, 40);
    lv_obj_set_style_text_line_space(s_dev_lbl, 6, 0);
}

static void build_wifi(lv_obj_t *p)
{
    lv_obj_t *title = label(p, &lv_font_montserrat_20, COL_TEXT);
    lv_label_set_text(title, LV_SYMBOL_WIFI "  Wi-Fi Export");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

    lv_obj_t *card = lv_obj_create(p);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 204, 120);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_bg_color(card, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_pad_all(card, 12, 0);

    lv_obj_t *k1 = label(card, &lv_font_montserrat_14, COL_DIM);
    lv_label_set_text(k1, "Network");
    s_wifi_ssid = label(card, &lv_font_montserrat_14, COL_TEXT);
    lv_obj_set_pos(s_wifi_ssid, 0, 18);
    lv_obj_t *k2 = label(card, &lv_font_montserrat_14, COL_DIM);
    lv_label_set_text(k2, "Password");
    lv_obj_set_pos(k2, 0, 44);
    s_wifi_pwd = label(card, &lv_font_montserrat_14, COL_TEXT);
    lv_obj_set_pos(s_wifi_pwd, 0, 62);

    lv_obj_t *ep = label(p, &lv_font_montserrat_14, COL_DIM);
    lv_label_set_text(ep, "wss://192.168.4.1:443");
    lv_obj_align(ep, LV_ALIGN_TOP_MID, 0, 170);
    s_wifi_state = label(p, &lv_font_montserrat_20, COL_ACCENT);
    lv_obj_align(s_wifi_state, LV_ALIGN_TOP_MID, 0, 196);
    s_wifi_count = label(p, &lv_font_montserrat_14, COL_DIM);
    lv_obj_align(s_wifi_count, LV_ALIGN_TOP_MID, 0, 226);
}

static void fmt_kb(char *buf, size_t cap, uint32_t kb)
{
    if (kb >= 1024) snprintf(buf, cap, "%lu.%lu MB", (unsigned long)(kb / 1024), (unsigned long)(kb % 1024 * 10 / 1024));
    else snprintf(buf, cap, "%lu KB", (unsigned long)kb);
}

static void update_header(const sc_snapshot_t *s)
{
    const char *link = "Starting";
    uint32_t color = COL_DIM;
    switch (s->link) {
    case SC_LINK_ADVERTISING: link = LV_SYMBOL_BLUETOOTH " Waiting for app"; break;
    case SC_LINK_CONNECTED: link = LV_SYMBOL_BLUETOOTH " Connected"; color = COL_OK; break;
    case SC_LINK_WIFI_PENDING: link = LV_SYMBOL_WIFI " Switching"; color = COL_ACCENT; break;
    case SC_LINK_WIFI_AP:
    case SC_LINK_WIFI_CLIENT: link = LV_SYMBOL_WIFI " Hotspot on"; color = COL_ACCENT; break;
    default: break;
    }
    lv_label_set_text(s_link_lbl, link);
    lv_obj_set_style_text_color(s_link_lbl, lv_color_hex(color), 0);

    if (s->battery_soc < 0) {
        lv_label_set_text(s_batt_lbl, LV_SYMBOL_BATTERY_EMPTY " --");
    } else {
        const char *sym = s->battery_soc > 80 ? LV_SYMBOL_BATTERY_FULL
                        : s->battery_soc > 55 ? LV_SYMBOL_BATTERY_3
                        : s->battery_soc > 30 ? LV_SYMBOL_BATTERY_2
                        : s->battery_soc > 10 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
        lv_label_set_text_fmt(s_batt_lbl, "%d%% %s", s->battery_soc, sym);
    }
}

static void update_recorder(const sc_snapshot_t *s)
{
    uint32_t color = COL_DIM;
    const char *state = "READY";
    if (s->rec_status == SC_REC_RECORDING) { color = COL_REC; state = "RECORDING"; }
    else if (s->rec_status == SC_REC_PAUSED) { color = COL_PAUSE; state = "PAUSED"; }
    lv_label_set_text(s_state_lbl, state);
    lv_obj_set_style_text_color(s_state_lbl, lv_color_hex(color), 0);
    lv_obj_set_style_arc_color(s_ring, lv_color_hex(color), LV_PART_INDICATOR);
    int ring = s->rec_status == SC_REC_RECORDING ? (s->level < 8 ? 8 : s->level)
             : s->rec_status == SC_REC_PAUSED ? 100 : 0;
    lv_arc_set_value(s_ring, ring);

    uint32_t sec = s->rec_seconds;
    if (sec >= 3600) lv_label_set_text_fmt(s_time_lbl, "%lu:%02lu:%02lu", (unsigned long)(sec / 3600),
                                           (unsigned long)(sec / 60 % 60), (unsigned long)(sec % 60));
    else lv_label_set_text_fmt(s_time_lbl, "%02lu:%02lu", (unsigned long)(sec / 60), (unsigned long)(sec % 60));

    if (s->streaming && s->streaming_realtime && s->rec_file) lv_label_set_text(s_file_lbl, "streaming to app");
    else if (s->streaming) lv_label_set_text_fmt(s_file_lbl, "sending %lu%%",
        (unsigned long)(s->xfer_total ? (uint64_t)s->xfer_sent * 100 / s->xfer_total : 0));
    else if (s->rec_file) lv_label_set_text_fmt(s_file_lbl, "#%lu", (unsigned long)s->rec_file);
    else lv_label_set_text(s_file_lbl, "");

    chip_set(s_chip_bound, s->bound, COL_ACCENT);
    chip_set(s_chip_secure, s->session, COL_OK);
    chip_set(s_chip_live, s->streaming, COL_REC);

    char used[16], total[16];
    fmt_kb(used, sizeof(used), s->total_kb > s->free_kb ? s->total_kb - s->free_kb : 0);
    fmt_kb(total, sizeof(total), s->total_kb);
    lv_label_set_text_fmt(s_store_lbl, "%lu files  -  %s / %s", (unsigned long)s->file_count, used, total);
    lv_bar_set_value(s_store_bar, s->total_kb ? (int32_t)((uint64_t)(s->total_kb - s->free_kb) * 1000 / s->total_kb) : 0,
                     LV_ANIM_OFF);
}

static void update_files(void)
{
    static sc_file_entry_t files[FILE_ROWS];
    size_t n = sc_core_recent_files(files, FILE_ROWS);
    lv_obj_set_hidden(s_files_empty, n != 0);
    for (int i = 0; i < FILE_ROWS; i++) {
        if ((size_t)i >= n) {
            lv_label_set_text(s_file_rows[i], "");
            continue;
        }
        uint32_t sec = files[i].size / SC_UNIT_DATA / 50u;
        time_t t = (time_t)files[i].file_id;
        struct tm tm;
        localtime_r(&t, &tm);
        lv_label_set_text_fmt(s_file_rows[i], "%02d-%02d %02d:%02d     %lu:%02lu", tm.tm_mon + 1, tm.tm_mday,
                              tm.tm_hour, tm.tm_min, (unsigned long)(sec / 60), (unsigned long)(sec % 60));
    }
}

static void update_device(const sc_snapshot_t *s)
{
    char total[16], free_s[16];
    fmt_kb(total, sizeof(total), s->total_kb);
    fmt_kb(free_s, sizeof(free_s), s->free_kb);
    lv_label_set_text_fmt(s_dev_lbl,
        "Model    D3200 (emulated)\n"
        "MAC      %02X:%02X:%02X:%02X:%02X:%02X\n"
        "SN       %s\n"
        "Firmware 01.00\n"
        "Bound    %s\n"
        "Session  %s\n"
        "Clock    %s\n"
        "Storage  %s free of %s\n"
        "Heap     %u KB free",
        s->mac[0], s->mac[1], s->mac[2], s->mac[3], s->mac[4], s->mac[5], s->serial,
        s->bound ? "yes" : "no", s->session ? "encrypted" : "none",
        s->time_synced ? "synced" : "not synced", free_s, total,
        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
}

static void update_wifi(const sc_snapshot_t *s)
{
    lv_label_set_text(s_wifi_ssid, s->ssid);
    lv_label_set_text(s_wifi_pwd, s->password[0] ? s->password : "(open)");
    const char *state = "Starting...";
    if (s->link == SC_LINK_WIFI_AP) state = "Join this network";
    else if (s->link == SC_LINK_WIFI_CLIENT) state = "Phone connected";
    lv_label_set_text(s_wifi_state, state);
    lv_label_set_text_fmt(s_wifi_count, "%lu exported  -  hold OK to stop", (unsigned long)s->wifi_files);
}

static void refresh(lv_timer_t *timer)
{
    (void)timer;
    sc_snapshot_t s;
    sc_core_snapshot(&s);
    update_header(&s);

    bool wifi = s.link >= SC_LINK_WIFI_PENDING;
    atomic_store(&s_wifi_shown, wifi);
    int page = atomic_load(&s_page_req);
    if (wifi) page = -2;
    if (page != s_page) {
        for (int i = 0; i < PAGE_COUNT; i++) {
            lv_obj_set_hidden(s_pages[i], i != page);
            lv_obj_set_style_bg_color(s_dots[i], lv_color_hex(i == page ? COL_TEXT : COL_CARD), 0);
        }
        lv_obj_set_hidden(s_wifi_page, !wifi);
        s_page = page;
    }

    if (wifi) update_wifi(&s);
    else if (page == PAGE_RECORDER) update_recorder(&s);
    else if (page == PAGE_FILES) update_files();
    else update_device(&s);

    if (s.toast[0]) {
        lv_label_set_text(s_hint_lbl, s.toast);
        lv_obj_set_style_text_color(s_hint_lbl, lv_color_hex(COL_TEXT), 0);
    } else {
        lv_label_set_text(s_hint_lbl, wifi ? "" : "OK rec/pause  -  hold OK save");
        lv_obj_set_style_text_color(s_hint_lbl, lv_color_hex(COL_DIM), 0);
    }
}

esp_err_t sc_ui_init(void)
{
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(s_scr, false);

    s_link_lbl = label(s_scr, &lv_font_montserrat_14, COL_DIM);
    lv_obj_set_pos(s_link_lbl, 26, 14);
    s_batt_lbl = label(s_scr, &lv_font_montserrat_14, COL_TEXT);
    lv_obj_align(s_batt_lbl, LV_ALIGN_TOP_RIGHT, -26, 14);

    for (int i = 0; i < PAGE_COUNT; i++) s_pages[i] = page_create(s_scr);
    s_wifi_page = page_create(s_scr);
    build_recorder(s_pages[PAGE_RECORDER]);
    build_files(s_pages[PAGE_FILES]);
    build_device(s_pages[PAGE_DEVICE]);
    build_wifi(s_wifi_page);

    for (int i = 0; i < PAGE_COUNT; i++) {
        s_dots[i] = lv_obj_create(s_scr);
        lv_obj_remove_style_all(s_dots[i]);
        lv_obj_set_size(s_dots[i], 6, 6);
        lv_obj_set_style_radius(s_dots[i], 3, 0);
        lv_obj_set_style_bg_opa(s_dots[i], LV_OPA_COVER, 0);
        lv_obj_set_pos(s_dots[i], 108 + i * 10, 282);
    }
    s_hint_lbl = label(s_scr, &lv_font_montserrat_14, COL_DIM);
    lv_obj_align(s_hint_lbl, LV_ALIGN_BOTTOM_MID, 0, -11);

    lv_screen_load(s_scr);
    refresh(NULL);
    lv_timer_create(refresh, 200, NULL);
    return ESP_OK;
}

void sc_ui_page_step(int delta)
{
    int p = atomic_load(&s_page_req);
    p = (p + delta + PAGE_COUNT) % PAGE_COUNT;
    atomic_store(&s_page_req, p);
}

bool sc_ui_wifi_page(void)
{
    return atomic_load(&s_wifi_shown);
}
