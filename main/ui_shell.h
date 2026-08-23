#pragma once

#include "lvgl.h"

/*
 * Phase 3: the real navigation shell -- a searchable/grouped host list plus
 * an add/edit form, replacing the Phase 1 test button. Call ui_shell_init()
 * once (inside the display lock, before the first screen is shown), passing
 * the keyboard indev so each screen can bind its own lv_group_t to it.
 */
void ui_shell_init(lv_indev_t *kbd_indev);
void ui_shell_show_home(void);

/* For other screens (e.g. session_ui.c's SSH password prompt) that need to
 * bind their own lv_group_t to the same physical keyboard. */
lv_indev_t *ui_shell_get_kbd_indev(void);

/* Shows msg as a one-time banner the next time (or right now, if already
 * showing) ui_shell_show_home() renders -- used by session_ui.c to explain
 * why a session ended (WiFi lost vs. the remote closing it) once control
 * returns to the host list. Pass NULL to clear without showing anything. */
void ui_shell_set_status_message(const char *msg);
