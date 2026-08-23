#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

/*
 * Raw key callback: bypasses the LVGL indev/group/focus path entirely.
 * While one is registered, every keypress (Tab included, delivered as a
 * literal '\t' instead of LV_KEY_NEXT) goes straight to the callback and is
 * NOT also enqueued for the LVGL indev -- used by the terminal session
 * screen, which wants raw bytes, not form-navigation semantics. Pass NULL
 * to go back to normal LVGL keypad behavior.
 */
typedef void (*keyboard_raw_key_cb_t)(uint32_t key, bool pressed);
void keyboard_indev_set_raw_cb(keyboard_raw_key_cb_t cb);

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
