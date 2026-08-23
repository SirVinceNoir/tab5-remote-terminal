#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "lvgl.h"

/*
 * Cell-grid VT100/ANSI terminal widget (Phase 4). Renders into an lv_canvas
 * using the built-in 16x16 monospace unscii font -- 80x42 cells at the
 * panel's native 1280 width, no scaling needed. 42 rows (672px) rather than
 * a full 45 (720px) deliberately leaves a 48px strip free at the top of the
 * screen for a session status bar (host name, local-echo toggle, disconnect)
 * -- see session_ui.c.
 *
 * v1 scope, deliberately: ASCII only (no UTF-8), and a reasonably capable
 * but non-exhaustive VT100/xterm subset (cursor movement, erase, 16-color
 * SGR, scroll regions, the DECSC/DECRC cursor save pair, and the
 * alt-screen-buffer modes vim/less/htop rely on). Unrecognized escape
 * sequences are consumed and discarded rather than left to corrupt the
 * parser or leak through as garbage text.
 *
 * Scrollback: lines that scroll off the top during normal full-screen
 * scrolling are retained (main screen only -- like a real terminal, the
 * alt screen vim/htop/less use has no scrollback) and browsable via
 * terminal_scroll(). Scrolling back only changes what's displayed; new
 * output keeps arriving into the live buffer underneath and the cursor is
 * hidden while scrolled back, matching real terminal behavior.
 */

#define TERM_COLS 80
#define TERM_ROWS 42
#define TERM_CHAR_W 16
#define TERM_CHAR_H 16

typedef struct terminal terminal_t;

terminal_t *terminal_create(lv_obj_t *parent);
lv_obj_t *terminal_get_widget(const terminal_t *term);

/* Parses raw bytes from the remote session into the in-memory cell grid.
 * Does not touch the display -- call terminal_render() afterward (while
 * holding bsp_display_lock()) to push the updated grid to the canvas. */
void terminal_feed(terminal_t *term, const uint8_t *data, size_t len);
void terminal_render(terminal_t *term);

/* True once the remote has asked for "application" cursor keys (DECCKM) --
 * changes which escape sequence arrow keys should send. */
bool terminal_app_cursor_keys(const terminal_t *term);

/* Scrolls the displayed viewport by delta_lines (positive = further back
 * into history, negative = toward the live bottom), clamped to what's
 * actually retained. Call terminal_render() afterward as usual. */
void terminal_scroll(terminal_t *term, int delta_lines);
bool terminal_is_scrolled(const terminal_t *term);

void terminal_destroy(terminal_t *term);
