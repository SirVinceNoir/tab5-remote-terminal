#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"
#include "host_store.h"
#include "ui_shell.h"
#include "ui_wifi.h"
#include "session_ui.h"
#include "wifi_manager.h"

static const char *TAG = "ui_shell";

static lv_indev_t *s_kbd_indev;
static lv_group_t *s_group; /* group bound to whichever screen is currently active */
static char s_status_message[128];

void ui_shell_set_status_message(const char *msg)
{
    if (msg == NULL) {
        s_status_message[0] = '\0';
    } else {
        strncpy(s_status_message, msg, sizeof(s_status_message) - 1);
        s_status_message[sizeof(s_status_message) - 1] = '\0';
    }
}

/* ---- Home screen (host list) ------------------------------------------ */

static lv_obj_t *s_search_ta;
static lv_obj_t *s_list_container;

static void show_edit_screen(int host_index);

static bool matches_filter(const host_entry_t *h, const char *needle)
{
    if (needle == NULL || needle[0] == '\0') {
        return true;
    }
    return (strcasestr(h->name, needle) != NULL) ||
           (strcasestr(h->group, needle) != NULL) ||
           (strcasestr(h->address, needle) != NULL);
}

static const char *protocol_str(host_protocol_t p)
{
    switch (p) {
    case HOST_PROTOCOL_SSH:    return "ssh";
    case HOST_PROTOCOL_TELNET: return "telnet";
    case HOST_PROTOCOL_SERIAL: return "serial";
    default:                   return "?";
    }
}

static void host_row_click_cb(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    const host_entry_t *h = host_store_get(index);
    ESP_LOGI(TAG, "tapped host '%s', protocol=%d (0=ssh,1=telnet,2=serial)",
              h != NULL ? h->name : "?", h != NULL ? (int)h->protocol : -1);
    if (h != NULL && h->protocol == HOST_PROTOCOL_TELNET) {
        session_ui_open_telnet(h);
        return;
    }
    if (h != NULL && h->protocol == HOST_PROTOCOL_SSH) {
        session_ui_open_ssh(h);
        return;
    }
    /* Serial sessions aren't wired up yet (Phase 6) -- fall back to edit,
     * same as a long-press, until they are. */
    show_edit_screen(index);
}

static void host_row_long_press_cb(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    show_edit_screen(index);
}

static void add_btn_click_cb(lv_event_t *e)
{
    show_edit_screen(-1);
}

static void wifi_btn_click_cb(lv_event_t *e)
{
    ui_wifi_show();
}

static void wifi_status_timer_cb(lv_timer_t *timer)
{
    lv_obj_t *lbl = (lv_obj_t *)lv_timer_get_user_data(timer);
    char ssid_buf[33];
    if (wifi_manager_get_status(ssid_buf, sizeof(ssid_buf))) {
        lv_label_set_text_fmt(lbl, "WiFi: %s", ssid_buf);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x00AA00), 0);
    } else {
        lv_label_set_text(lbl, "WiFi: disconnected");
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xCC0000), 0);
    }
}

static void wifi_status_timer_delete_cb(lv_event_t *e)
{
    lv_timer_delete((lv_timer_t *)lv_event_get_user_data(e));
}

static void rebuild_host_list(void)
{
    lv_obj_clean(s_list_container);

    const char *filter = lv_textarea_get_text(s_search_ta);
    int count = host_store_count();

    /* Group hosts by first-seen group name; "" (ungrouped) hosts render
     * under a literal "Ungrouped" header at the end. Small N (<= 64) makes
     * the O(n^2) bucketing below perfectly fine. Heap-allocated (not a local
     * array) because HOST_STORE_MAX_HOSTS * HOST_GROUP_MAX_LEN (4KB) blew the
     * "main" task's stack when this ran during initial app_main() setup. */
    char (*seen_groups)[HOST_GROUP_MAX_LEN] = malloc(HOST_STORE_MAX_HOSTS * HOST_GROUP_MAX_LEN);
    if (seen_groups == NULL) {
        ESP_LOGE(TAG, "out of memory building host list");
        return;
    }
    int seen_group_count = 0;
    bool any_ungrouped = false;

    for (int i = 0; i < count; i++) {
        const host_entry_t *h = host_store_get(i);
        if (!matches_filter(h, filter)) {
            continue;
        }
        if (h->group[0] == '\0') {
            any_ungrouped = true;
            continue;
        }
        bool known = false;
        for (int g = 0; g < seen_group_count; g++) {
            if (strcmp(seen_groups[g], h->group) == 0) {
                known = true;
                break;
            }
        }
        if (!known && seen_group_count < HOST_STORE_MAX_HOSTS) {
            strncpy(seen_groups[seen_group_count], h->group, HOST_GROUP_MAX_LEN - 1);
            seen_groups[seen_group_count][HOST_GROUP_MAX_LEN - 1] = '\0';
            seen_group_count++;
        }
    }

    bool rendered_any = false;

    for (int g = 0; g < seen_group_count; g++) {
        lv_obj_t *header = lv_label_create(s_list_container);
        lv_label_set_text(header, seen_groups[g]);
        lv_obj_set_style_text_font(header, lv_theme_get_font_small(s_list_container), 0);

        for (int i = 0; i < count; i++) {
            const host_entry_t *h = host_store_get(i);
            if (!matches_filter(h, filter) || strcmp(h->group, seen_groups[g]) != 0) {
                continue;
            }
            lv_obj_t *btn = lv_button_create(s_list_container);
            lv_obj_set_width(btn, LV_PCT(100));
            lv_group_add_obj(s_group, btn);
            lv_obj_add_event_cb(btn, host_row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            lv_obj_add_event_cb(btn, host_row_long_press_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);

            lv_obj_t *lbl = lv_label_create(btn);
            lv_label_set_text_fmt(lbl, "%s  %s://%s:%u", h->name, protocol_str(h->protocol),
                                   h->address, h->port);
            rendered_any = true;
        }
    }

    if (any_ungrouped) {
        lv_obj_t *header = lv_label_create(s_list_container);
        lv_label_set_text(header, "Ungrouped");
        lv_obj_set_style_text_font(header, lv_theme_get_font_small(s_list_container), 0);

        for (int i = 0; i < count; i++) {
            const host_entry_t *h = host_store_get(i);
            if (!matches_filter(h, filter) || h->group[0] != '\0') {
                continue;
            }
            lv_obj_t *btn = lv_button_create(s_list_container);
            lv_obj_set_width(btn, LV_PCT(100));
            lv_group_add_obj(s_group, btn);
            lv_obj_add_event_cb(btn, host_row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            lv_obj_add_event_cb(btn, host_row_long_press_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);

            lv_obj_t *lbl = lv_label_create(btn);
            lv_label_set_text_fmt(lbl, "%s  %s://%s:%u", h->name, protocol_str(h->protocol),
                                   h->address, h->port);
            rendered_any = true;
        }
    }

    if (!rendered_any) {
        lv_obj_t *empty = lv_label_create(s_list_container);
        lv_label_set_text(empty, count == 0 ? "No hosts yet -- tap + to add one"
                                             : "No hosts match your search");
    }

    free(seen_groups);
}

static void search_ta_event_cb(lv_event_t *e)
{
    rebuild_host_list();
}

void ui_shell_show_home(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, 18, 0);
    lv_obj_set_style_pad_gap(scr, 12, 0);

    lv_group_t *old_group = s_group;
    s_group = lv_group_create();

    lv_obj_t *top_bar = lv_obj_create(scr);
    lv_obj_set_size(top_bar, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(top_bar, 12, 0);
    lv_obj_set_style_pad_all(top_bar, 0, 0);

    lv_obj_t *title = lv_label_create(top_bar);
    lv_label_set_text(title, "Hosts");

    s_search_ta = lv_textarea_create(top_bar);
    lv_textarea_set_one_line(s_search_ta, true);
    lv_textarea_set_placeholder_text(s_search_ta, "Search...");
    lv_obj_set_flex_grow(s_search_ta, 1);
    lv_group_add_obj(s_group, s_search_ta);
    lv_obj_add_event_cb(s_search_ta, search_ta_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *wifi_status_lbl = lv_label_create(top_bar);
    lv_timer_t *wifi_status_timer = lv_timer_create(wifi_status_timer_cb, 3000, wifi_status_lbl);
    wifi_status_timer_cb(wifi_status_timer); /* paint the real state now, don't wait out the first period */
    /* Ties the timer's lifetime to this screen's own deletion (whether via
     * an explicit delete or ui_shell_show_home()'s next auto_del screen
     * swap) so it can never fire again after wifi_status_lbl is freed. */
    lv_obj_add_event_cb(scr, wifi_status_timer_delete_cb, LV_EVENT_DELETE, wifi_status_timer);

    lv_obj_t *wifi_btn = lv_button_create(top_bar);
    lv_obj_t *wifi_lbl = lv_label_create(wifi_btn);
    lv_label_set_text(wifi_lbl, "WiFi");
    lv_group_add_obj(s_group, wifi_btn);
    lv_obj_add_event_cb(wifi_btn, wifi_btn_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *add_btn = lv_button_create(top_bar);
    lv_obj_t *add_lbl = lv_label_create(add_btn);
    lv_label_set_text(add_lbl, "+ Add");
    lv_group_add_obj(s_group, add_btn);
    lv_obj_add_event_cb(add_btn, add_btn_click_cb, LV_EVENT_CLICKED, NULL);

    if (s_status_message[0] != '\0') {
        lv_obj_t *banner = lv_label_create(scr);
        lv_label_set_text(banner, s_status_message);
        lv_obj_set_style_text_color(banner, lv_color_hex(0xFF8800), 0);
        s_status_message[0] = '\0'; /* one-shot -- don't repeat it on the next visit */
    }

    s_list_container = lv_obj_create(scr);
    lv_obj_set_size(s_list_container, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_grow(s_list_container, 1);
    lv_obj_set_flex_flow(s_list_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(s_list_container, 6, 0);

    rebuild_host_list();

    lv_screen_load_anim(scr, LV_SCREEN_LOAD_ANIM_NONE, 0, 0, true);
    lv_indev_set_group(s_kbd_indev, s_group);

    if (old_group != NULL) {
        lv_group_delete(old_group);
    }
}

/* ---- Add/edit host screen ---------------------------------------------- */

static struct {
    int index; /* -1 = adding a new host */
    lv_obj_t *name_ta;
    lv_obj_t *group_ta;
    lv_obj_t *protocol_dd;
    lv_obj_t *address_ta;
    lv_obj_t *port_ta;
    lv_obj_t *username_ta;
    lv_obj_t *auth_dd;
    lv_obj_t *keepalive_ta;
} s_edit;

static void save_btn_click_cb(lv_event_t *e)
{
    host_entry_t h = { 0 };
    strncpy(h.name, lv_textarea_get_text(s_edit.name_ta), sizeof(h.name) - 1);
    strncpy(h.group, lv_textarea_get_text(s_edit.group_ta), sizeof(h.group) - 1);
    h.protocol = (host_protocol_t)lv_dropdown_get_selected(s_edit.protocol_dd);
    strncpy(h.address, lv_textarea_get_text(s_edit.address_ta), sizeof(h.address) - 1);
    h.port = (uint16_t)atoi(lv_textarea_get_text(s_edit.port_ta));
    strncpy(h.username, lv_textarea_get_text(s_edit.username_ta), sizeof(h.username) - 1);
    h.auth_type = (host_auth_type_t)lv_dropdown_get_selected(s_edit.auth_dd);
    h.keepalive_sec = (uint16_t)atoi(lv_textarea_get_text(s_edit.keepalive_ta));

    if (h.name[0] == '\0') {
        ESP_LOGW(TAG, "refusing to save a host with an empty name");
        return;
    }

    esp_err_t ret = (s_edit.index < 0) ? host_store_add(&h) : host_store_update(s_edit.index, &h);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to save host: %s", esp_err_to_name(ret));
        return;
    }
    ui_shell_show_home();
}

static void delete_btn_click_cb(lv_event_t *e)
{
    if (s_edit.index >= 0) {
        host_store_delete(s_edit.index);
    }
    ui_shell_show_home();
}

static void cancel_btn_click_cb(lv_event_t *e)
{
    ui_shell_show_home();
}

static uint16_t default_port_for_protocol(host_protocol_t p)
{
    switch (p) {
    case HOST_PROTOCOL_SSH:    return 22;
    case HOST_PROTOCOL_TELNET: return 23;
    default:                   return 0;
    }
}

static void protocol_dd_changed_cb(lv_event_t *e)
{
    if (s_edit.index >= 0) {
        return; /* only auto-fill the port when adding a new host, never
                  * clobber a port the user already set on an existing one */
    }
    char buf[8];
    snprintf(buf, sizeof(buf), "%u", default_port_for_protocol((host_protocol_t)lv_dropdown_get_selected(s_edit.protocol_dd)));
    lv_textarea_set_text(s_edit.port_ta, buf);
}

static lv_obj_t *add_field(lv_obj_t *parent, lv_group_t *group, const char *label_text)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_gap(row, 12, 0);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, label_text);
    lv_obj_set_width(label, 165);

    lv_obj_t *ta = lv_textarea_create(row);
    lv_textarea_set_one_line(ta, true);
    lv_obj_set_flex_grow(ta, 1);
    lv_group_add_obj(group, ta);
    return ta;
}

static void show_edit_screen(int host_index)
{
    const host_entry_t *existing = (host_index >= 0) ? host_store_get(host_index) : NULL;
    s_edit.index = host_index;

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, 18, 0);
    lv_obj_set_style_pad_gap(scr, 12, 0);

    lv_group_t *old_group = s_group;
    s_group = lv_group_create();

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, existing != NULL ? "Edit host" : "Add host");

    s_edit.name_ta = add_field(scr, s_group, "Name");
    s_edit.group_ta = add_field(scr, s_group, "Group");

    lv_obj_t *proto_row = lv_obj_create(scr);
    lv_obj_set_size(proto_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(proto_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(proto_row, 0, 0);
    lv_obj_set_style_pad_gap(proto_row, 12, 0);
    lv_obj_t *proto_label = lv_label_create(proto_row);
    lv_label_set_text(proto_label, "Protocol");
    lv_obj_set_width(proto_label, 165);
    s_edit.protocol_dd = lv_dropdown_create(proto_row);
    lv_dropdown_set_options(s_edit.protocol_dd, "ssh\ntelnet\nserial");
    lv_obj_set_flex_grow(s_edit.protocol_dd, 1);
    lv_group_add_obj(s_group, s_edit.protocol_dd);
    lv_obj_add_event_cb(s_edit.protocol_dd, protocol_dd_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_edit.address_ta = add_field(scr, s_group, "Address");
    s_edit.port_ta = add_field(scr, s_group, "Port");
    lv_textarea_set_accepted_chars(s_edit.port_ta, "0123456789");
    s_edit.username_ta = add_field(scr, s_group, "Username");

    lv_obj_t *auth_row = lv_obj_create(scr);
    lv_obj_set_size(auth_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(auth_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(auth_row, 0, 0);
    lv_obj_set_style_pad_gap(auth_row, 12, 0);
    lv_obj_t *auth_label = lv_label_create(auth_row);
    lv_label_set_text(auth_label, "Auth");
    lv_obj_set_width(auth_label, 165);
    s_edit.auth_dd = lv_dropdown_create(auth_row);
    lv_dropdown_set_options(s_edit.auth_dd, "password\nkey");
    lv_obj_set_flex_grow(s_edit.auth_dd, 1);
    lv_group_add_obj(s_group, s_edit.auth_dd);

    s_edit.keepalive_ta = add_field(scr, s_group, "Keepalive (s)");
    lv_textarea_set_accepted_chars(s_edit.keepalive_ta, "0123456789");

    if (existing != NULL) {
        lv_textarea_set_text(s_edit.name_ta, existing->name);
        lv_textarea_set_text(s_edit.group_ta, existing->group);
        lv_dropdown_set_selected(s_edit.protocol_dd, existing->protocol);
        lv_textarea_set_text(s_edit.address_ta, existing->address);
        char buf[16];
        snprintf(buf, sizeof(buf), "%u", existing->port);
        lv_textarea_set_text(s_edit.port_ta, buf);
        lv_textarea_set_text(s_edit.username_ta, existing->username);
        lv_dropdown_set_selected(s_edit.auth_dd, existing->auth_type);
        snprintf(buf, sizeof(buf), "%u", existing->keepalive_sec);
        lv_textarea_set_text(s_edit.keepalive_ta, buf);
    } else {
        char port_buf[8];
        snprintf(port_buf, sizeof(port_buf), "%u",
                  default_port_for_protocol((host_protocol_t)lv_dropdown_get_selected(s_edit.protocol_dd)));
        lv_textarea_set_text(s_edit.port_ta, port_buf);
        lv_textarea_set_text(s_edit.keepalive_ta, "0");
    }

    lv_obj_t *btn_row = lv_obj_create(scr);
    lv_obj_set_size(btn_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_set_style_pad_gap(btn_row, 12, 0);

    lv_obj_t *save_btn = lv_button_create(btn_row);
    lv_obj_t *save_lbl = lv_label_create(save_btn);
    lv_label_set_text(save_lbl, "Save");
    lv_group_add_obj(s_group, save_btn);
    lv_obj_add_event_cb(save_btn, save_btn_click_cb, LV_EVENT_CLICKED, NULL);

    if (existing != NULL) {
        lv_obj_t *delete_btn = lv_button_create(btn_row);
        lv_obj_t *delete_lbl = lv_label_create(delete_btn);
        lv_label_set_text(delete_lbl, "Delete");
        lv_group_add_obj(s_group, delete_btn);
        lv_obj_add_event_cb(delete_btn, delete_btn_click_cb, LV_EVENT_CLICKED, NULL);
    }

    lv_obj_t *cancel_btn = lv_button_create(btn_row);
    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_group_add_obj(s_group, cancel_btn);
    lv_obj_add_event_cb(cancel_btn, cancel_btn_click_cb, LV_EVENT_CLICKED, NULL);

    lv_screen_load_anim(scr, LV_SCREEN_LOAD_ANIM_NONE, 0, 0, true);
    lv_indev_set_group(s_kbd_indev, s_group);

    if (old_group != NULL) {
        lv_group_delete(old_group);
    }
}

void ui_shell_init(lv_indev_t *kbd_indev)
{
    s_kbd_indev = kbd_indev;
}

lv_indev_t *ui_shell_get_kbd_indev(void)
{
    return s_kbd_indev;
}
