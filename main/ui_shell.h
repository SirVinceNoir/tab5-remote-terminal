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
