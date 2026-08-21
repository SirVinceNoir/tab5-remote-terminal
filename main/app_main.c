#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "wifi_manager.h"
#include "keyboard_indev.h"
#include "ssh_spike.h"
#include "usb_serial_spike.h"
#include "host_store.h"
#include "ui_shell.h"
#include "ui_wifi.h"

static const char *TAG = "tab5_remote_terminal";

void app_main(void)
{
    ESP_LOGI(TAG, "Tab5 Remote Terminal boot");

    ESP_ERROR_CHECK(host_store_init());

    lv_display_t *disp = bsp_display_start();
    if (disp == NULL) {
        ESP_LOGE(TAG, "bsp_display_start() failed");
        return;
    }

    /* Panel's native scan is 720x1280 portrait; rotate to landscape. */
    bsp_display_rotate(disp, LV_DISPLAY_ROTATION_90);

    bsp_display_lock(0);

    lv_indev_t *kbd_indev = keyboard_indev_start();
    ui_shell_init(kbd_indev);
    ui_wifi_init(kbd_indev);
    ui_shell_show_home();

    bsp_display_unlock();
    bsp_display_backlight_on();

    ESP_LOGI(TAG, "display + touch + keyboard bring-up ready");

    usb_serial_spike_start();

    wifi_manager_init();
    if (wifi_manager_connect_saved(15000)) {
        ssh_spike_start();
    } else {
        ESP_LOGI(TAG, "no saved WiFi credentials (or connect failed) -- "
                       "use the WiFi button on the home screen to set one up");
    }
}
