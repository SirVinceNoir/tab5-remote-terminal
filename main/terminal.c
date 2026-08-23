#include <string.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "terminal.h"

static const char *TAG = "terminal";

#define CHAR_W TERM_CHAR_W
#define CHAR_H TERM_CHAR_H
#define CANVAS_W (TERM_COLS * CHAR_W)
#define CANVAS_H (TERM_ROWS * CHAR_H)
#define CSI_BUF_LEN 64
#define SCROLLBACK_LINES 300

#define DEFAULT_FG 7
#define DEFAULT_BG 0

#define ATTR_BOLD    0x01
#define ATTR_UNDERL  0x02
#define ATTR_REVERSE 0x04

typedef struct {
    char ch;
    uint8_t fg;
    uint8_t bg;
    uint8_t attrs;
} term_cell_t;

typedef enum { TSTATE_NORMAL, TSTATE_ESC, TSTATE_ESC_CHARSET, TSTATE_CSI, TSTATE_OSC } term_state_t;

struct terminal {
    lv_obj_t *canvas;
    void *canvas_buf;

    term_cell_t *grid; /* points at main_grid or alt_grid -- whichever is active */
    term_cell_t *main_grid;
    term_cell_t *alt_grid;
    bool alt_active;

    int cur_row, cur_col;
    uint8_t cur_fg, cur_bg, cur_attrs;
    bool cursor_visible;

    int saved_row, saved_col;
    uint8_t saved_fg, saved_bg, saved_attrs;

    int scroll_top, scroll_bottom; /* inclusive, 0-based */
    bool pending_wrap; /* deferred autowrap -- see term_putc()'s printable-char path */

    /* Scrollback: a ring buffer of lines that scrolled off the top of the
     * main screen (never the alt screen -- see terminal.h). scrollback_head
     * is the next write slot; the newest stored line is (head - 1). */
    term_cell_t *scrollback;
    int scrollback_head;
    int scrollback_count; /* valid lines currently retained, <= SCROLLBACK_LINES */
    int scroll_offset;    /* 0 = live view; >0 = N lines back into history */
    int rendered_scroll_offset;

    term_state_t state;
    char csi_buf[CSI_BUF_LEN];
    int csi_len;
    bool csi_private;

    bool app_cursor_keys;

    /* Shadow of what's currently actually on the canvas. terminal_render()
     * diffs each row against this and skips redrawing rows that haven't
     * changed -- full-grid redraws on every render() call (regardless of
     * how little changed) were the dominant cost behind a very low
     * perceived frame rate. Content-diffing rather than instrumenting every
     * mutation site with dirty-flags is deliberate: it can't miss a spot. */
    term_cell_t *rendered_grid;
    int rendered_cursor_row, rendered_cursor_col;
    bool rendered_cursor_visible;

    /* Scratch space for terminal_render()'s per-run label text -- persistent
     * (not a loop-local stack array) so every lv_draw_label() call in a pass
     * gets a stable, uniquely-addressed slice that stays valid regardless of
     * whether the draw backend rasterizes immediately or defers within the
     * layer, and there's no aliasing between runs drawn earlier in the same
     * pass. Re-used fully fresh every render() call. */
    char *text_scratch;
};

/* Classic xterm 16-color palette. */
static const uint32_t PALETTE[16] = {
    0x000000, 0xCD0000, 0x00CD00, 0xCDCD00,
    0x0000EE, 0xCD00CD, 0x00CDCD, 0xE5E5E5,
    0x7F7F7F, 0xFF0000, 0x00FF00, 0xFFFF00,
    0x5C5CFF, 0xFF00FF, 0x00FFFF, 0xFFFFFF,
};

static void cell_clear_range(term_cell_t *grid, int row, int col_from, int col_to,
                              uint8_t fg, uint8_t bg)
{
    for (int c = col_from; c <= col_to && c < TERM_COLS; c++) {
        term_cell_t *cell = &grid[row * TERM_COLS + c];
        cell->ch = ' ';
        cell->fg = fg;
        cell->bg = bg;
        cell->attrs = 0;
    }
}

static void grid_clear_all(term_cell_t *grid, uint8_t fg, uint8_t bg)
{
    for (int r = 0; r < TERM_ROWS; r++) {
        cell_clear_range(grid, r, 0, TERM_COLS - 1, fg, bg);
    }
}

static void term_reset_state(terminal_t *t)
{
    t->cur_row = 0;
    t->cur_col = 0;
    t->cur_fg = DEFAULT_FG;
    t->cur_bg = DEFAULT_BG;
    t->cur_attrs = 0;
    t->cursor_visible = true;
    t->scroll_top = 0;
    t->scroll_bottom = TERM_ROWS - 1;
    t->app_cursor_keys = false;
    t->state = TSTATE_NORMAL;
    t->csi_len = 0;
    t->csi_private = false;
    t->pending_wrap = false;
}

terminal_t *terminal_create(lv_obj_t *parent)
{
    terminal_t *t = calloc(1, sizeof(terminal_t));
    if (t == NULL) {
        return NULL;
    }

    t->main_grid = calloc(TERM_ROWS * TERM_COLS, sizeof(term_cell_t));
    t->alt_grid  = calloc(TERM_ROWS * TERM_COLS, sizeof(term_cell_t));
    t->rendered_grid = malloc(TERM_ROWS * TERM_COLS * sizeof(term_cell_t));
    t->scrollback = calloc(SCROLLBACK_LINES * TERM_COLS, sizeof(term_cell_t));
    t->text_scratch = malloc(TERM_ROWS * TERM_COLS + 1); /* +1 for the cursor overlay's single char */
    if (t->main_grid == NULL || t->alt_grid == NULL || t->rendered_grid == NULL ||
        t->scrollback == NULL || t->text_scratch == NULL) {
        ESP_LOGE(TAG, "out of memory allocating terminal grid");
        free(t->main_grid);
        free(t->alt_grid);
        free(t->rendered_grid);
        free(t->scrollback);
        free(t->text_scratch);
        free(t);
        return NULL;
    }
    t->grid = t->main_grid;
    /* 0xFF can't occur in a real cell (chars are ASCII 0x20-0x7E), so every
     * cell mismatches on the first render -- a guaranteed full redraw
     * without a separate "is this the first call" flag. */
    memset(t->rendered_grid, 0xFF, TERM_ROWS * TERM_COLS * sizeof(term_cell_t));
    t->rendered_cursor_row = -1;
    t->rendered_cursor_col = -1;
    t->rendered_scroll_offset = -1;

    size_t buf_size = (size_t)CANVAS_W * CANVAS_H * 2; /* RGB565 */
    t->canvas_buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (t->canvas_buf == NULL) {
        ESP_LOGE(TAG, "out of memory allocating %u byte canvas buffer", (unsigned)buf_size);
        free(t->main_grid);
        free(t->alt_grid);
        free(t->rendered_grid);
        free(t->scrollback);
        free(t->text_scratch);
        free(t);
        return NULL;
    }

    t->canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(t->canvas, t->canvas_buf, CANVAS_W, CANVAS_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_size(t->canvas, CANVAS_W, CANVAS_H);

    term_reset_state(t);
    grid_clear_all(t->main_grid, DEFAULT_FG, DEFAULT_BG);
    grid_clear_all(t->alt_grid, DEFAULT_FG, DEFAULT_BG);

    return t;
}

lv_obj_t *terminal_get_widget(const terminal_t *t)
{
    return t->canvas;
}

bool terminal_app_cursor_keys(const terminal_t *t)
{
    return t->app_cursor_keys;
}

void terminal_scroll(terminal_t *t, int delta_lines)
{
    int new_offset = t->scroll_offset + delta_lines;
    if (new_offset < 0) {
        new_offset = 0;
    }
    if (new_offset > t->scrollback_count) {
        new_offset = t->scrollback_count;
    }
    t->scroll_offset = new_offset;
}

bool terminal_is_scrolled(const terminal_t *t)
{
    return t->scroll_offset > 0;
}

/* Resolves a screen row (0 = top of viewport) to the cell row that should be
 * displayed there, accounting for scroll_offset. At offset 0 this is just
 * the live grid; scrolled back, the top portion comes from scrollback and
 * the rest from the top of the live grid, in strict chronological order. */
static const term_cell_t *display_row(const terminal_t *t, int screen_row)
{
    int logical = (t->scrollback_count - t->scroll_offset) + screen_row;
    if (logical < t->scrollback_count) {
        int newest_index = (t->scrollback_head - 1 + SCROLLBACK_LINES) % SCROLLBACK_LINES;
        int offset_from_newest = (t->scrollback_count - 1) - logical; /* 0 = newest retained line */
        int phys = (newest_index - offset_from_newest + SCROLLBACK_LINES) % SCROLLBACK_LINES;
        return &t->scrollback[phys * TERM_COLS];
    }
    int grid_row = logical - t->scrollback_count;
    return &t->grid[grid_row * TERM_COLS];
}

static void push_scrollback(terminal_t *t, const term_cell_t *row)
{
    if (t->alt_active) {
        return; /* real terminals don't retain scrollback for the alt screen */
    }
    memcpy(&t->scrollback[t->scrollback_head * TERM_COLS], row, TERM_COLS * sizeof(term_cell_t));
    t->scrollback_head = (t->scrollback_head + 1) % SCROLLBACK_LINES;
    if (t->scrollback_count < SCROLLBACK_LINES) {
        t->scrollback_count++;
    }
    /* Keep a currently-scrolled-back user looking at the same historical
     * content rather than having it silently shift under them by one line. */
    if (t->scroll_offset > 0) {
        t->scroll_offset++;
        if (t->scroll_offset > t->scrollback_count) {
            t->scroll_offset = t->scrollback_count;
        }
    }
}

static void term_scroll_region_up(terminal_t *t, int top, int bottom)
{
    if (top == 0 && bottom == TERM_ROWS - 1) {
        /* Only a full-screen scroll feeds scrollback -- a scroll region
         * confined to part of the screen (e.g. a fixed status line) isn't
         * "normal" scrolling and real terminals don't retain it either. */
        push_scrollback(t, &t->grid[0]);
    }
    for (int r = top; r < bottom; r++) {
        memcpy(&t->grid[r * TERM_COLS], &t->grid[(r + 1) * TERM_COLS], TERM_COLS * sizeof(term_cell_t));
    }
    cell_clear_range(t->grid, bottom, 0, TERM_COLS - 1, t->cur_fg, t->cur_bg);
}

static void term_line_feed(terminal_t *t)
{
    if (t->cur_row == t->scroll_bottom) {
        term_scroll_region_up(t, t->scroll_top, t->scroll_bottom);
    } else if (t->cur_row < TERM_ROWS - 1) {
        t->cur_row++;
    }
}

static void term_erase_in_line(terminal_t *t, int mode)
{
    switch (mode) {
    case 1: cell_clear_range(t->grid, t->cur_row, 0, t->cur_col, t->cur_fg, t->cur_bg); break;
    case 2: cell_clear_range(t->grid, t->cur_row, 0, TERM_COLS - 1, t->cur_fg, t->cur_bg); break;
    default: cell_clear_range(t->grid, t->cur_row, t->cur_col, TERM_COLS - 1, t->cur_fg, t->cur_bg); break;
    }
}

static void term_erase_in_display(terminal_t *t, int mode)
{
    switch (mode) {
    case 1:
        for (int r = 0; r < t->cur_row; r++) {
            cell_clear_range(t->grid, r, 0, TERM_COLS - 1, t->cur_fg, t->cur_bg);
        }
        cell_clear_range(t->grid, t->cur_row, 0, t->cur_col, t->cur_fg, t->cur_bg);
        break;
    case 2:
    case 3:
        grid_clear_all(t->grid, t->cur_fg, t->cur_bg);
        break;
    default:
        cell_clear_range(t->grid, t->cur_row, t->cur_col, TERM_COLS - 1, t->cur_fg, t->cur_bg);
        for (int r = t->cur_row + 1; r < TERM_ROWS; r++) {
            cell_clear_range(t->grid, r, 0, TERM_COLS - 1, t->cur_fg, t->cur_bg);
        }
        break;
    }
}

static void term_apply_sgr(terminal_t *t, const int *params, int count)
{
    if (count == 0) {
        t->cur_fg = DEFAULT_FG;
        t->cur_bg = DEFAULT_BG;
        t->cur_attrs = 0;
        return;
    }
    for (int i = 0; i < count; i++) {
        int p = params[i] < 0 ? 0 : params[i];
        if (p == 0) { t->cur_fg = DEFAULT_FG; t->cur_bg = DEFAULT_BG; t->cur_attrs = 0; }
        else if (p == 1) { t->cur_attrs |= ATTR_BOLD; }
        else if (p == 4) { t->cur_attrs |= ATTR_UNDERL; }
        else if (p == 7) { t->cur_attrs |= ATTR_REVERSE; }
        else if (p == 22) { t->cur_attrs &= ~ATTR_BOLD; }
        else if (p == 24) { t->cur_attrs &= ~ATTR_UNDERL; }
        else if (p == 27) { t->cur_attrs &= ~ATTR_REVERSE; }
        else if (p >= 30 && p <= 37) { t->cur_fg = (uint8_t)(p - 30); }
        else if (p == 39) { t->cur_fg = DEFAULT_FG; }
        else if (p >= 40 && p <= 47) { t->cur_bg = (uint8_t)(p - 40); }
        else if (p == 49) { t->cur_bg = DEFAULT_BG; }
        else if (p >= 90 && p <= 97) { t->cur_fg = (uint8_t)(8 + (p - 90)); }
        else if (p >= 100 && p <= 107) { t->cur_bg = (uint8_t)(8 + (p - 100)); }
        /* italic/blink/etc. intentionally ignored -- no v1 rendering for them */
    }
}

static void term_set_private_mode(terminal_t *t, int mode, bool enable)
{
    switch (mode) {
    case 1:
        t->app_cursor_keys = enable;
        break;
    case 25:
        t->cursor_visible = enable;
        break;
    case 47:
    case 1047:
    case 1049:
        if (enable && !t->alt_active) {
            t->alt_active = true;
            t->grid = t->alt_grid;
            grid_clear_all(t->grid, DEFAULT_FG, DEFAULT_BG);
            if (mode == 1049) {
                t->saved_row = t->cur_row;
                t->saved_col = t->cur_col;
            }
            t->cur_row = 0;
            t->cur_col = 0;
        } else if (!enable && t->alt_active) {
            t->alt_active = false;
            t->grid = t->main_grid;
            if (mode == 1049) {
                t->cur_row = t->saved_row;
                t->cur_col = t->saved_col;
            }
        }
        break;
    default:
        break; /* mouse reporting, bracketed paste, etc. -- not implemented */
    }
}

static int parse_params(const char *buf, int len, int *out, int max_out)
{
    int count = 0;
    int i = 0;
    while (i < len && count < max_out) {
        if (buf[i] == ';') {
            out[count++] = -1;
            i++;
            continue;
        }
        int val = 0;
        bool any_digit = false;
        while (i < len && buf[i] >= '0' && buf[i] <= '9') {
            val = val * 10 + (buf[i] - '0');
            any_digit = true;
            i++;
        }
        out[count++] = any_digit ? val : -1;
        if (i < len && buf[i] == ';') {
            i++;
        }
    }
    return count;
}

static void term_dispatch_csi(terminal_t *t, char final)
{
    int params[16];
    int count = parse_params(t->csi_buf, t->csi_len, params, 16);
    int p0 = (count > 0 && params[0] >= 0) ? params[0] : 0;
    int p0_or1 = (p0 == 0) ? 1 : p0;

    /* Any CSI sequence -- private-mode or not -- is explicit terminal
     * control and cancels a deferred autowrap (see term_putc()); notably
     * the alt-screen-switch modes below reset cur_row/cur_col outright, so
     * a stale pending wrap must not survive into the new screen. */
    t->pending_wrap = false;

    if (t->csi_private) {
        if (final == 'h' || final == 'l') {
            for (int i = 0; i < count; i++) {
                term_set_private_mode(t, params[i] < 0 ? 0 : params[i], final == 'h');
            }
        }
        return;
    }

    switch (final) {
    case 'A':
        t->cur_row -= p0_or1;
        if (t->cur_row < t->scroll_top) t->cur_row = t->scroll_top;
        break;
    case 'B':
        t->cur_row += p0_or1;
        if (t->cur_row > t->scroll_bottom) t->cur_row = t->scroll_bottom;
        break;
    case 'C':
        t->cur_col += p0_or1;
        if (t->cur_col > TERM_COLS - 1) t->cur_col = TERM_COLS - 1;
        break;
    case 'D':
        t->cur_col -= p0_or1;
        if (t->cur_col < 0) t->cur_col = 0;
        break;
    case 'H':
    case 'f': {
        int row = (count > 0 && params[0] > 0) ? params[0] - 1 : 0;
        int col = (count > 1 && params[1] > 0) ? params[1] - 1 : 0;
        t->cur_row = row < 0 ? 0 : (row >= TERM_ROWS ? TERM_ROWS - 1 : row);
        t->cur_col = col < 0 ? 0 : (col >= TERM_COLS ? TERM_COLS - 1 : col);
        break;
    }
    case 'J':
        term_erase_in_display(t, p0);
        break;
    case 'K':
        term_erase_in_line(t, p0);
        break;
    case 'm':
        term_apply_sgr(t, params, count);
        break;
    case 'r': {
        int top = (count > 0 && params[0] > 0) ? params[0] - 1 : 0;
        int bottom = (count > 1 && params[1] > 0) ? params[1] - 1 : TERM_ROWS - 1;
        if (top < 0) top = 0;
        if (bottom >= TERM_ROWS) bottom = TERM_ROWS - 1;
        if (top < bottom) {
            t->scroll_top = top;
            t->scroll_bottom = bottom;
        } else {
            t->scroll_top = 0;
            t->scroll_bottom = TERM_ROWS - 1;
        }
        t->cur_row = t->scroll_top;
        t->cur_col = 0;
        break;
    }
    default:
        break; /* unimplemented final byte -- ignore, don't corrupt future parsing */
    }
}


static void term_putc(terminal_t *t, uint8_t c)
{
    switch (t->state) {
    case TSTATE_ESC:
        t->state = TSTATE_NORMAL;
        if (c == '[') {
            t->state = TSTATE_CSI;
            t->csi_len = 0;
            t->csi_private = false;
        } else if (c == ']') {
            t->state = TSTATE_OSC;
        } else if (c == '(' || c == ')' || c == '*' || c == '+') {
            /* G0-G3 charset designation (e.g. ESC ( 0 for DEC line-drawing,
             * which htop/vim/less use constantly for box borders) always
             * takes exactly one more byte -- consume it so it can't leak
             * through as a stray printed character. Not applying the
             * designated charset is an acceptable v1 gap (ASCII only). */
            t->state = TSTATE_ESC_CHARSET;
        } else if (c == '7') {
            t->saved_row = t->cur_row;
            t->saved_col = t->cur_col;
            t->saved_fg = t->cur_fg;
            t->saved_bg = t->cur_bg;
            t->saved_attrs = t->cur_attrs;
        } else if (c == '8') {
            t->cur_row = t->saved_row;
            t->cur_col = t->saved_col;
            t->cur_fg = t->saved_fg;
            t->cur_bg = t->saved_bg;
            t->cur_attrs = t->saved_attrs;
        } else if (c == 'c') {
            term_reset_state(t);
            grid_clear_all(t->grid, DEFAULT_FG, DEFAULT_BG);
        }
        return;

    case TSTATE_ESC_CHARSET:
        t->state = TSTATE_NORMAL;
        return;

    case TSTATE_CSI:
        if (c == '?' && t->csi_len == 0) {
            t->csi_private = true;
            return;
        }
        if ((c >= '0' && c <= '9') || c == ';') {
            if (t->csi_len < CSI_BUF_LEN - 1) {
                t->csi_buf[t->csi_len++] = (char)c;
            }
            return;
        }
        t->state = TSTATE_NORMAL;
        term_dispatch_csi(t, (char)c);
        return;

    case TSTATE_OSC:
        if (c == 0x07 || c == 0x1B) {
            t->state = TSTATE_NORMAL;
        }
        return;

    case TSTATE_NORMAL:
    default:
        break;
    }

    switch (c) {
    case 0x1B: t->state = TSTATE_ESC; return;
    case '\r': t->cur_col = 0; t->pending_wrap = false; return;
    case '\n': t->pending_wrap = false; term_line_feed(t); return;
    case '\b': t->pending_wrap = false; if (t->cur_col > 0) t->cur_col--; return;
    case '\t':
        t->pending_wrap = false;
        t->cur_col = ((t->cur_col / 8) + 1) * 8;
        if (t->cur_col > TERM_COLS - 1) t->cur_col = TERM_COLS - 1;
        return;
    case 0x07: return; /* BEL -- no bell in v1 */
    default: break;
    }

    if (c < 0x20 || c == 0x7F || c >= 0x80) {
        return; /* other control chars, and non-ASCII (no UTF-8 in v1) */
    }

    /* Deferred ("pending") autowrap, matching real terminal behavior: when
     * the last column of a row gets filled, the cursor does NOT move yet --
     * it only advances (with a linefeed) right before the NEXT printable
     * character is written. Advancing eagerly (as soon as the last column
     * was filled) made the terminal jump to the next row before a program
     * that had assumed deferred-wrap semantics got a chance to reposition
     * the cursor itself (e.g. a CR or cursor-move immediately after filling
     * the line), which is what caused wrapped long lines to visually land
     * on top of / overwrite whatever the program wrote right after them. */
    if (t->pending_wrap) {
        t->cur_col = 0;
        term_line_feed(t);
        t->pending_wrap = false;
    }

    term_cell_t *cell = &t->grid[t->cur_row * TERM_COLS + t->cur_col];
    cell->ch = (char)c;
    cell->fg = t->cur_fg;
    cell->bg = t->cur_bg;
    cell->attrs = t->cur_attrs;

    if (t->cur_col == TERM_COLS - 1) {
        t->pending_wrap = true;
    } else {
        t->cur_col++;
    }
}

void terminal_feed(terminal_t *t, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        term_putc(t, data[i]);
    }
}

static bool row_unchanged(const terminal_t *t, int r)
{
    return memcmp(display_row(t, r), &t->rendered_grid[r * TERM_COLS],
                  TERM_COLS * sizeof(term_cell_t)) == 0;
}

void terminal_render(terminal_t *t)
{
    bool show_cursor = t->cursor_visible && t->scroll_offset == 0;
    bool scroll_changed = t->scroll_offset != t->rendered_scroll_offset;
    bool cursor_changed = t->cur_row != t->rendered_cursor_row ||
                          t->cur_col != t->rendered_cursor_col ||
                          show_cursor != t->rendered_cursor_visible;

    bool any_row_changed = cursor_changed || scroll_changed;
    for (int r = 0; r < TERM_ROWS && !any_row_changed; r++) {
        if (!row_unchanged(t, r)) {
            any_row_changed = true;
        }
    }
    if (!any_row_changed) {
        return; /* nothing visible actually changed since the last render */
    }

    lv_layer_t layer;
    lv_canvas_init_layer(t->canvas, &layer);

    lv_draw_rect_dsc_t rect_dsc;
    lv_draw_label_dsc_t label_dsc;
    int scratch_pos = 0;

    for (int r = 0; r < TERM_ROWS; r++) {
        /* Skip rows that are pixel-identical to what's already on the
         * canvas -- unless the cursor is (or was) on this row (it needs to
         * be drawn or erased even when the text didn't change), or the
         * scroll position just changed (every row's logical source may
         * have shifted, not worth diffing that case). */
        if (!scroll_changed && r != t->cur_row && r != t->rendered_cursor_row && row_unchanged(t, r)) {
            continue;
        }
        const term_cell_t *src_row = display_row(t, r);
        memcpy(&t->rendered_grid[r * TERM_COLS], src_row, TERM_COLS * sizeof(term_cell_t));

        int c = 0;
        while (c < TERM_COLS) {
            const term_cell_t *first = &src_row[c];
            int run_len = 1;
            while (c + run_len < TERM_COLS) {
                const term_cell_t *next = &src_row[c + run_len];
                if (next->fg != first->fg || next->bg != first->bg || next->attrs != first->attrs) {
                    break;
                }
                run_len++;
            }

            bool reverse = (first->attrs & ATTR_REVERSE) != 0;
            uint8_t fg_idx = reverse ? first->bg : first->fg;
            uint8_t bg_idx = reverse ? first->fg : first->bg;
            lv_color_t fg_color = lv_color_hex(PALETTE[fg_idx]);
            lv_color_t bg_color = lv_color_hex(PALETTE[bg_idx]);

            lv_area_t area = {
                .x1 = c * CHAR_W, .y1 = r * CHAR_H,
                .x2 = (c + run_len) * CHAR_W - 1, .y2 = (r + 1) * CHAR_H - 1,
            };

            lv_draw_rect_dsc_init(&rect_dsc);
            rect_dsc.bg_color = bg_color;
            rect_dsc.bg_opa = LV_OPA_COVER;
            lv_draw_rect(&layer, &rect_dsc, &area);

            char *run_text = &t->text_scratch[scratch_pos];
            for (int i = 0; i < run_len; i++) {
                char ch = src_row[c + i].ch;
                run_text[i] = (ch == '\0') ? ' ' : ch;
            }
            scratch_pos += run_len;

            lv_draw_label_dsc_init(&label_dsc);
            label_dsc.color = fg_color;
            label_dsc.font = &lv_font_unscii_16;
            label_dsc.text = run_text;
            label_dsc.text_length = (uint32_t)run_len;
            lv_draw_label(&layer, &label_dsc, &area);

            c += run_len;
        }
    }

    if (show_cursor) {
        const term_cell_t *cell = &t->grid[t->cur_row * TERM_COLS + t->cur_col];
        lv_area_t area = {
            .x1 = t->cur_col * CHAR_W, .y1 = t->cur_row * CHAR_H,
            .x2 = (t->cur_col + 1) * CHAR_W - 1, .y2 = (t->cur_row + 1) * CHAR_H - 1,
        };

        lv_draw_rect_dsc_init(&rect_dsc);
        rect_dsc.bg_color = lv_color_hex(PALETTE[cell->fg]);
        rect_dsc.bg_opa = LV_OPA_COVER;
        lv_draw_rect(&layer, &rect_dsc, &area);

        char *cursor_ch = &t->text_scratch[scratch_pos];
        *cursor_ch = (cell->ch == '\0') ? ' ' : cell->ch;
        lv_draw_label_dsc_init(&label_dsc);
        label_dsc.color = lv_color_hex(PALETTE[cell->bg]);
        label_dsc.font = &lv_font_unscii_16;
        label_dsc.text = cursor_ch;
        label_dsc.text_length = 1;
        lv_draw_label(&layer, &label_dsc, &area);
    }

    lv_canvas_finish_layer(t->canvas, &layer);

    t->rendered_cursor_row = t->cur_row;
    t->rendered_cursor_col = t->cur_col;
    t->rendered_cursor_visible = show_cursor;
    t->rendered_scroll_offset = t->scroll_offset;
}

void terminal_destroy(terminal_t *t)
{
    if (t == NULL) {
        return;
    }
    if (t->canvas != NULL) {
        lv_obj_delete(t->canvas);
    }
    free(t->canvas_buf);
    free(t->main_grid);
    free(t->alt_grid);
    free(t->rendered_grid);
    free(t->scrollback);
    free(t->text_scratch);
    free(t);
}
