#pragma once

#include "lvgl.h"

/*
 * Phase 3: the real LVGL keypad input device for the A164 keyboard,
 * replacing the Phase 2 keyboard_spike.c throwaway driver. Reads Normal
 * mode's press/release + row/col events (unchanged from the spike) and
 * translates them via a ported HID keymap into either LV_KEY_* navigation
 * codes or literal characters, feeding an lv_indev_t of type
 * LV_INDEV_TYPE_KEYPAD.
 *
 * Call after bsp_display_start() (needs an active LVGL display to attach
 * the indev to). Returns the created indev so the caller can route it to
 * an lv_group_t via lv_indev_set_group() once the navigation shell has one.
 */
lv_indev_t *keyboard_indev_start(void);
