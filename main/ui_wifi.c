#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "bsp/esp-bsp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_manager.h"
#include "ui_shell.h"
#include "ui_wifi.h"

static const char *TAG = "ui_wifi";

static lv_indev_t *s_kbd_indev;
static lv_group_t *s_group;

static lv_obj_t *s_list_container;
static lv_obj_t *s_status_label;

static wifi_ap_record_t s_scan_results[WIFI_MANAGER_MAX_SCAN_RESULTS];
static int s_scan_count;

static lv_obj_t *s_pw_ta;
static char s_pw_ssid[33];

static void show_scan_screen(void);
static void show_password_screen(int ap_index);
static void start_connect(const char *ssid, const char *password);

/* ---- connect (runs off the LVGL task -- scan/connect block for seconds) - */

typedef struct {
    char ssid[33];
    char password[65];
} connect_args_t;

static void connect_task(void *arg)
{
    connect_args_t *args = (connect_args_t *)arg;
    bool ok = wifi_manager_connect(args->ssid, args->password, 15000);

    bsp_display_lock(0);
    if (ok) {
        ui_shell_show_home();
    } else if (s_status_label != NULL) {
        lv_label_set_text(s_status_label, "Connect failed -- check the password and try again");
    }
    bsp_display_unlock();

    free(args);
    vTaskDelete(NULL);
}

static void start_connect(const char *ssid, const char *password)
{
    connect_args_t *args = malloc(sizeof(*args));
    if (args == NULL) {
        ESP_LOGE(TAG, "out of memory starting WiFi connect");
        return;
    }
    memset(args, 0, sizeof(*args));
    strncpy(args->ssid, ssid, sizeof(args->ssid) - 1);
    strncpy(args->password, password, sizeof(args->password) - 1);

    if (s_status_label != NULL) {
        lv_label_set_text_fmt(s_status_label, "Connecting to '%s'...", ssid);
    }

    xTaskCreate(connect_task, "wifi_ui_connect", 4096, args, tskIDLE_PRIORITY + 1, NULL);
}

/* ---- scan screen -------------------------------------------------------- */

static void ap_row_click_cb(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    if (index < 0 || index >= s_scan_count) {
        return;
    }
    const wifi_ap_record_t *ap = &s_scan_results[index];
    if (ap->authmode == WIFI_AUTH_OPEN) {
        start_connect((const char *)ap->ssid, "");
    } else {
        show_password_screen(index);
    }
}

static void scan_task(void *arg)
{
    int count = wifi_manager_scan(s_scan_results, WIFI_MANAGER_MAX_SCAN_RESULTS);
    s_scan_count = count;

    bsp_display_lock(0);
    lv_obj_clean(s_list_container);

    if (count == 0) {
        lv_label_set_text(s_status_label, "No networks found -- tap Rescan to try again");
    } else {
        lv_label_set_text_fmt(s_status_label, "%d network(s) found", count);
        for (int i = 0; i < count; i++) {
            const wifi_ap_record_t *ap = &s_scan_results[i];

            lv_obj_t *btn = lv_button_create(s_list_container);
            lv_obj_set_width(btn, LV_PCT(100));
            lv_group_add_obj(s_group, btn);
            lv_obj_add_event_cb(btn, ap_row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

            lv_obj_t *lbl = lv_label_create(btn);
            lv_label_set_text_fmt(lbl, "%s  %ddBm%s", (const char *)ap->ssid, ap->rssi,
                                   ap->authmode == WIFI_AUTH_OPEN ? "" : "  (secured)");
        }
    }

    bsp_display_unlock();
    vTaskDelete(NULL);
}

static void rescan_btn_click_cb(lv_event_t *e)
{
    show_scan_screen();
}

static void wifi_back_btn_click_cb(lv_event_t *e)
{
    ui_shell_show_home();
}

static void show_scan_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, 12, 0);
    lv_obj_set_style_pad_gap(scr, 8, 0);

    lv_group_t *old_group = s_group;
    s_group = lv_group_create();

    lv_obj_t *top_bar = lv_obj_create(scr);
    lv_obj_set_size(top_bar, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(top_bar, 8, 0);
    lv_obj_set_style_pad_all(top_bar, 0, 0);

    lv_obj_t *title = lv_label_create(top_bar);
    lv_label_set_text(title, "WiFi networks");
    lv_obj_set_flex_grow(title, 1);

    lv_obj_t *rescan_btn = lv_button_create(top_bar);
    lv_obj_t *rescan_lbl = lv_label_create(rescan_btn);
    lv_label_set_text(rescan_lbl, "Rescan");
    lv_group_add_obj(s_group, rescan_btn);
    lv_obj_add_event_cb(rescan_btn, rescan_btn_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_btn = lv_button_create(top_bar);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, "Back");
    lv_group_add_obj(s_group, back_btn);
    lv_obj_add_event_cb(back_btn, wifi_back_btn_click_cb, LV_EVENT_CLICKED, NULL);

    s_status_label = lv_label_create(scr);
    lv_label_set_text(s_status_label, "Scanning...");

    s_list_container = lv_obj_create(scr);
    lv_obj_set_size(s_list_container, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_grow(s_list_container, 1);
    lv_obj_set_flex_flow(s_list_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(s_list_container, 4, 0);

    lv_screen_load(scr);
    lv_indev_set_group(s_kbd_indev, s_group);

    if (old_group != NULL) {
        lv_group_delete(old_group);
    }

    xTaskCreate(scan_task, "wifi_ui_scan", 4096, NULL, tskIDLE_PRIORITY + 1, NULL);
}

/* ---- password screen ----------------------------------------------------- */

static void connect_btn_click_cb(lv_event_t *e)
{
    const char *password = lv_textarea_get_text(s_pw_ta);
    start_connect(s_pw_ssid, password);
}

static void pw_cancel_btn_click_cb(lv_event_t *e)
{
    show_scan_screen();
}

static void show_password_screen(int ap_index)
{
    const wifi_ap_record_t *ap = &s_scan_results[ap_index];
    strncpy(s_pw_ssid, (const char *)ap->ssid, sizeof(s_pw_ssid) - 1);
    s_pw_ssid[sizeof(s_pw_ssid) - 1] = '\0';

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, 12, 0);
    lv_obj_set_style_pad_gap(scr, 8, 0);

    lv_group_t *old_group = s_group;
    s_group = lv_group_create();

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text_fmt(title, "Password for '%s'", s_pw_ssid);

    s_pw_ta = lv_textarea_create(scr);
    lv_textarea_set_one_line(s_pw_ta, true);
    lv_textarea_set_password_mode(s_pw_ta, true);
    lv_textarea_set_placeholder_text(s_pw_ta, "Password");
    lv_obj_set_width(s_pw_ta, LV_PCT(100));
    lv_group_add_obj(s_group, s_pw_ta);

    s_status_label = lv_label_create(scr);
    lv_label_set_text(s_status_label, "");

    lv_obj_t *btn_row = lv_obj_create(scr);
    lv_obj_set_size(btn_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_set_style_pad_gap(btn_row, 8, 0);

    lv_obj_t *connect_btn = lv_button_create(btn_row);
    lv_obj_t *connect_lbl = lv_label_create(connect_btn);
    lv_label_set_text(connect_lbl, "Connect");
    lv_group_add_obj(s_group, connect_btn);
    lv_obj_add_event_cb(connect_btn, connect_btn_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cancel_btn = lv_button_create(btn_row);
    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_group_add_obj(s_group, cancel_btn);
    lv_obj_add_event_cb(cancel_btn, pw_cancel_btn_click_cb, LV_EVENT_CLICKED, NULL);

    lv_screen_load(scr);
    lv_indev_set_group(s_kbd_indev, s_group);

    if (old_group != NULL) {
        lv_group_delete(old_group);
    }
}

/* ---- public API ----------------------------------------------------------- */

void ui_wifi_show(void)
{
    show_scan_screen();
}

void ui_wifi_init(lv_indev_t *kbd_indev)
{
    s_kbd_indev = kbd_indev;
}
