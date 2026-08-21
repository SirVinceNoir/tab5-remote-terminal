#pragma once

#include "lvgl.h"

/*
 * WiFi settings screen: scan -> pick network -> password entry -> connect.
 * Call ui_wifi_init() once alongside ui_shell_init(), then ui_wifi_show()
 * whenever the user taps the WiFi button on the home screen.
 */
void ui_wifi_init(lv_indev_t *kbd_indev);
void ui_wifi_show(void);
