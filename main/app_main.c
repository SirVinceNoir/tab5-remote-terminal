#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "wifi_spike.h"
#include "keyboard_indev.h"
#include "ssh_spike.h"
#include "usb_serial_spike.h"
#include "host_store.h"

static const char *TAG = "tab5_remote_terminal";

static void screen_touch_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) {
        return;
    }
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    ESP_LOGI(TAG, "touch: x=%d y=%d", (int)point.x, (int)point.y);
}

static void counter_btn_cb(lv_event_t *e)
{
    static int count = 0;
    count++;

    lv_obj_t *label = (lv_obj_t *)lv_event_get_user_data(e);
    lv_label_set_text_fmt(label, "Touched %d times", count);

    lv_obj_t *btn = lv_event_get_target(e);
    lv_color_t color = (count % 2 == 0) ? lv_palette_main(LV_PALETTE_BLUE) : lv_palette_main(LV_PALETTE_GREEN);
    lv_obj_set_style_bg_color(btn, color, 0);

    ESP_LOGI(TAG, "button clicked, count=%d", count);
}

static const char *protocol_name(host_protocol_t p)
{
    switch (p) {
    case HOST_PROTOCOL_SSH:    return "ssh";
    case HOST_PROTOCOL_TELNET: return "telnet";
    case HOST_PROTOCOL_SERIAL: return "serial";
    default:                   return "?";
    }
}

/* Temporary self-test for Phase 3's storage layer: seeds two sample hosts
 * on first boot, then logs the full list every boot. If LittleFS
 * persistence works, the seeded hosts should NOT be re-added after a
 * reflash-free reboot (host_store_count() will already be > 0). Will be
 * replaced by the real add/edit/list UI. */
static void host_store_selftest(void)
{
    ESP_ERROR_CHECK(host_store_init());

    if (host_store_count() == 0) {
        ESP_LOGI(TAG, "host list empty -- seeding sample hosts");
        host_store_add(&(host_entry_t){
            .name = "Signage PC", .group = "Test", .protocol = HOST_PROTOCOL_SSH,
            .address = "10.1.3.236", .port = 22, .username = "signage",
            .auth_type = HOST_AUTH_PASSWORD, .keepalive_sec = 30,
        });
        host_store_add(&(host_entry_t){
            .name = "Office Router", .group = "Test", .protocol = HOST_PROTOCOL_TELNET,
            .address = "10.1.2.1", .port = 23, .username = "admin",
            .auth_type = HOST_AUTH_PASSWORD, .keepalive_sec = 0,
        });
    }

    ESP_LOGI(TAG, "host store: %d host(s)", host_store_count());
    for (int i = 0; i < host_store_count(); i++) {
        const host_entry_t *h = host_store_get(i);
        ESP_LOGI(TAG, "  [%d] '%s' group='%s' %s://%s:%u user=%s",
                  i, h->name, h->group, protocol_name(h->protocol),
                  h->address, h->port, h->username);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Tab5 Remote Terminal boot");

    lv_display_t *disp = bsp_display_start();
    if (disp == NULL) {
        ESP_LOGE(TAG, "bsp_display_start() failed");
        return;
    }

    /* Panel's native scan is 720x1280 portrait; rotate to landscape. */
    bsp_display_rotate(disp, LV_DISPLAY_ROTATION_90);

    bsp_display_lock(0);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_add_event_cb(scr, screen_touch_cb, LV_EVENT_PRESSING, NULL);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Tab5 Remote Terminal - Phase 1 bring-up");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t *btn = lv_button_create(scr);
    lv_obj_set_size(btn, 260, 100);
    lv_obj_center(btn);

    lv_obj_t *btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "Touched 0 times");
    lv_obj_center(btn_label);

    lv_obj_add_event_cb(btn, counter_btn_cb, LV_EVENT_CLICKED, btn_label);

    /* Default group: the upcoming navigation shell's focusable widgets join
     * this automatically. Add the test button now so pressing Enter on the
     * keyboard exercises the same indev path real widgets will use. */
    lv_group_t *input_group = lv_group_create();
    lv_group_set_default(input_group);
    lv_group_add_obj(input_group, btn);

    lv_indev_t *kbd_indev = keyboard_indev_start();
    if (kbd_indev != NULL) {
        lv_indev_set_group(kbd_indev, input_group);
    }

    bsp_display_unlock();
    bsp_display_backlight_on();

    ESP_LOGI(TAG, "display + touch bring-up ready");

    usb_serial_spike_start();
    host_store_selftest();

    if (wifi_spike_start()) {
        ssh_spike_start();
    }
}
