#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "bsp/esp-bsp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "terminal.h"
#include "telnet_client.h"
#include "ssh_session.h"
#include "keyboard_indev.h"
#include "ui_shell.h"
#include "wifi_manager.h"
#include "session_ui.h"

static const char *TAG = "session_ui";

/* 3 terminal-row-heights (48px) -- matches terminal.h's TERM_ROWS choice,
 * which leaves exactly this much free at the top of the 720px screen. */
#define SESSION_STATUS_BAR_H 48
#define SCROLLBACK_JUMP_LINES 100000 /* larger than any real scrollback depth -- terminal_scroll() clamps it */

typedef struct {
    uint32_t key;
    bool pressed;
} session_key_event_t;

static QueueHandle_t s_key_queue;
static terminal_t *s_term;
static session_transport_t s_transport;
static volatile bool s_stop_requested;
static bool s_local_echo;
static lv_obj_t *s_echo_btn_lbl;
static lv_obj_t *s_info_lbl;
static lv_obj_t *s_jump_live_btn;

/* Touch-drag scrolling: a real content redraw is expensive (measured
 * 150-190ms -- a hard cost of this LVGL SW-rendering config that neither
 * throttling the redraw rate nor consolidating draw calls managed to
 * reduce, the latter having made it both slower AND briefly broken
 * rendering correctness). Rather than trying to make redraws faster, the
 * drag visually pans the ALREADY-RENDERED canvas image in real time via a
 * cheap style transform (no redraw at all) so it tracks the finger
 * smoothly, and only pays the real redraw cost periodically -- when enough
 * pixels have drifted that a growing blank gap would become visible, or on
 * release, which also resets the transform back to 0 since the freshly
 * redrawn content is already at the correct (untransformed) position. */
static int s_drag_pan_px;    /* accumulated pixels not yet folded into a real scroll+redraw */
static int64_t s_last_scroll_render_us;
#define SCROLL_RENDER_MIN_INTERVAL_US 40000 /* don't force a catch-up redraw more than ~25/s even during a fast flick */
#define SCROLL_PAN_CATCHUP_PX (TERM_CHAR_H * 10) /* beyond this much drift, catch up regardless of timing */
static char s_session_host_name[HOST_NAME_MAX_LEN];
static char s_session_protocol_label[16];

static void update_session_info_label(void)
{
    char ssid_buf[33];
    if (wifi_manager_get_status(ssid_buf, sizeof(ssid_buf))) {
        lv_label_set_text_fmt(s_info_lbl, "%s (%s) | WiFi: %s",
                                s_session_host_name, s_session_protocol_label, ssid_buf);
    } else {
        lv_label_set_text_fmt(s_info_lbl, "%s (%s) | WiFi: disconnected",
                                s_session_host_name, s_session_protocol_label);
    }
}

static void raw_key_cb(uint32_t key, bool pressed)
{
    if (!pressed) {
        return; /* only forward key-down, matching the LVGL widgets' own convention */
    }
    session_key_event_t evt = { .key = key, .pressed = pressed };
    xQueueSend(s_key_queue, &evt, 0);
}

static void send_key(uint32_t key)
{
    bool app_mode = terminal_app_cursor_keys(s_term);

    switch (key) {
    case LV_KEY_ENTER:
        s_transport.write(s_transport.ctx, (const uint8_t *)"\r\n", 2);
        return;
    case LV_KEY_BACKSPACE: {
        uint8_t b = 0x7F; /* DEL -- what most Unix ttys expect for Backspace */
        s_transport.write(s_transport.ctx, &b, 1);
        return;
    }
    case LV_KEY_ESC: {
        uint8_t b = 0x1B;
        s_transport.write(s_transport.ctx, &b, 1);
        return;
    }
    case LV_KEY_UP:
        s_transport.write(s_transport.ctx, (const uint8_t *)(app_mode ? "\x1BOA" : "\x1B[A"), 3);
        return;
    case LV_KEY_DOWN:
        s_transport.write(s_transport.ctx, (const uint8_t *)(app_mode ? "\x1BOB" : "\x1B[B"), 3);
        return;
    case LV_KEY_RIGHT:
        s_transport.write(s_transport.ctx, (const uint8_t *)(app_mode ? "\x1BOC" : "\x1B[C"), 3);
        return;
    case LV_KEY_LEFT:
        s_transport.write(s_transport.ctx, (const uint8_t *)(app_mode ? "\x1BOD" : "\x1B[D"), 3);
        return;
    case LV_KEY_DEL:
        s_transport.write(s_transport.ctx, (const uint8_t *)"\x1B[3~", 4);
        return;
    default:
        if (key < 0x100) {
            uint8_t b = (uint8_t)key;
            s_transport.write(s_transport.ctx, &b, 1);
        }
        return;
    }
}

/* Mirrors send_key()'s byte choices back into our own terminal, for targets
 * (like raw/bare TCP services) that don't echo back what you type -- most
 * real telnetd/sshd servers already echo remotely, so this defaults off. */
static void local_echo_feed(uint32_t key)
{
    switch (key) {
    case LV_KEY_ENTER:
        terminal_feed(s_term, (const uint8_t *)"\r\n", 2);
        return;
    case LV_KEY_BACKSPACE:
        terminal_feed(s_term, (const uint8_t *)"\b \b", 3);
        return;
    case LV_KEY_ESC:
        return; /* no visual effect for a bare Escape */
    case LV_KEY_UP:
        terminal_feed(s_term, (const uint8_t *)(terminal_app_cursor_keys(s_term) ? "\x1BOA" : "\x1B[A"), 3);
        return;
    case LV_KEY_DOWN:
        terminal_feed(s_term, (const uint8_t *)(terminal_app_cursor_keys(s_term) ? "\x1BOB" : "\x1B[B"), 3);
        return;
    case LV_KEY_RIGHT:
        terminal_feed(s_term, (const uint8_t *)(terminal_app_cursor_keys(s_term) ? "\x1BOC" : "\x1B[C"), 3);
        return;
    case LV_KEY_LEFT:
        terminal_feed(s_term, (const uint8_t *)(terminal_app_cursor_keys(s_term) ? "\x1BOD" : "\x1B[D"), 3);
        return;
    case LV_KEY_DEL:
        terminal_feed(s_term, (const uint8_t *)"\x1B[3~", 4);
        return;
    default:
        if (key < 0x100) {
            uint8_t b = (uint8_t)key;
            terminal_feed(s_term, &b, 1);
        }
        return;
    }
}

static void update_echo_btn_label(void)
{
    lv_label_set_text(s_echo_btn_lbl, s_local_echo ? "Echo: On" : "Echo: Off");
}

static void echo_toggle_btn_click_cb(lv_event_t *e)
{
    s_local_echo = !s_local_echo;
    update_echo_btn_label();
}

static void disconnect_btn_click_cb(lv_event_t *e)
{
    s_stop_requested = true; /* session_io_task notices within one read-timeout tick */
}

static void update_scroll_indicator(void)
{
    if (terminal_is_scrolled(s_term)) {
        lv_obj_remove_flag(s_jump_live_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_jump_live_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

static void jump_to_live_btn_click_cb(lv_event_t *e)
{
    terminal_scroll(s_term, -SCROLLBACK_JUMP_LINES);
    terminal_render(s_term);
    s_drag_pan_px = 0;
    lv_obj_set_style_translate_y(terminal_get_widget(s_term), 0, 0);
    update_scroll_indicator();
}

/* Folds any whole lines out of the accumulated pan into a real scroll +
 * redraw, then re-applies the (now sub-line) remainder as the visual pan
 * offset -- so the catch-up redraw doesn't cause a visible pop, and any
 * fractional-pixel drag position carries forward smoothly. */
static void catch_up_scroll(lv_obj_t *canvas)
{
    int lines = s_drag_pan_px / TERM_CHAR_H;
    if (lines != 0) {
        terminal_scroll(s_term, lines);
        s_drag_pan_px -= lines * TERM_CHAR_H;
    }
    s_last_scroll_render_us = esp_timer_get_time();
    terminal_render(s_term);
    lv_obj_set_style_translate_y(canvas, s_drag_pan_px, 0);
    update_scroll_indicator();
}

/* Touch-drag scrolling: content follows the finger, like a normal scrolling
 * list (drag down -> reveal earlier lines, drag up -> reveal later ones).
 * The visual pan (translate_y) happens on every event -- cheap, immediate,
 * no redraw -- and only gets folded into a real scroll+redraw periodically
 * (see catch_up_scroll()), so the drag tracks the finger smoothly even
 * though each real redraw is slow. */
static void canvas_pressing_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) {
        return;
    }
    lv_point_t vect;
    lv_indev_get_vect(indev, &vect);
    if (vect.y == 0) {
        return;
    }

    lv_obj_t *canvas = terminal_get_widget(s_term);
    s_drag_pan_px += vect.y;
    lv_obj_set_style_translate_y(canvas, s_drag_pan_px, 0);

    int64_t now = esp_timer_get_time();
    bool time_to_catch_up = (now - s_last_scroll_render_us) >= SCROLL_RENDER_MIN_INTERVAL_US;
    bool pan_too_far = s_drag_pan_px >= SCROLL_PAN_CATCHUP_PX || s_drag_pan_px <= -SCROLL_PAN_CATCHUP_PX;
    if (!time_to_catch_up && !pan_too_far) {
        return;
    }
    catch_up_scroll(canvas);
}

static void canvas_released_cb(lv_event_t *e)
{
    lv_obj_t *canvas = terminal_get_widget(s_term);
    catch_up_scroll(canvas);
    /* Fully settle -- no lingering sub-pixel pan once the gesture is over. */
    s_drag_pan_px = 0;
    lv_obj_set_style_translate_y(canvas, 0, 0);
}

static void session_io_task(void *arg)
{
    uint8_t buf[512];
    bool remote_closed = false;
    bool last_wifi_ok = wifi_manager_is_connected();

    while (!s_stop_requested) {
        int n = s_transport.read(s_transport.ctx, buf, sizeof(buf), 100);
        if (n > 0) {
            bsp_display_lock(0);
            terminal_feed(s_term, buf, (size_t)n);

            /* Coalesce: drain whatever's already arrived (zero-timeout, so
             * this never blocks waiting for more) before rendering once, so
             * a burst of output (fastfetch, ls, a fast redraw) costs one
             * render instead of one per ~512-byte chunk -- redrawing was
             * the dominant cost behind a low perceived frame rate. Capped
             * so a firehose can't starve keystroke sending indefinitely. */
            for (int drains = 0; drains < 8; drains++) {
                int n2 = s_transport.read(s_transport.ctx, buf, sizeof(buf), 0);
                if (n2 <= 0) {
                    break;
                }
                terminal_feed(s_term, buf, (size_t)n2);
            }

            terminal_render(s_term);
            bsp_display_unlock();
        } else if (n < 0) {
            ESP_LOGI(TAG, "remote closed the connection");
            remote_closed = true;
            break;
        }

        bool wifi_ok = wifi_manager_is_connected();
        if (wifi_ok != last_wifi_ok) {
            last_wifi_ok = wifi_ok;
            bsp_display_lock(0);
            update_session_info_label();
            bsp_display_unlock();
        }

        session_key_event_t evt;
        while (xQueueReceive(s_key_queue, &evt, 0) == pdTRUE) {
            if (terminal_is_scrolled(s_term)) {
                /* Typing while scrolled back into history snaps back to
                 * live, matching real terminals -- otherwise a keystroke's
                 * effect would land somewhere the user can't currently see. */
                bsp_display_lock(0);
                terminal_scroll(s_term, -SCROLLBACK_JUMP_LINES);
                terminal_render(s_term);
                s_drag_pan_px = 0;
                lv_obj_set_style_translate_y(terminal_get_widget(s_term), 0, 0);
                update_scroll_indicator();
                bsp_display_unlock();
            }
            send_key(evt.key);
            if (s_local_echo) {
                bsp_display_lock(0);
                local_echo_feed(evt.key);
                terminal_render(s_term);
                bsp_display_unlock();
            }
        }
    }

    keyboard_indev_set_raw_cb(NULL);
    s_transport.close(s_transport.ctx);

    if (remote_closed) {
        ui_shell_set_status_message(wifi_manager_is_connected()
                                          ? "Session ended: the remote closed the connection"
                                          : "Session ended: WiFi connection was lost");
    }

    bsp_display_lock(0);
    /* Must destroy the terminal (which deletes its canvas, a child of this
     * still-current screen) BEFORE loading the home screen -- ui_shell_show_home()
     * auto-deletes this screen once the new one loads, and deleting the same
     * canvas object twice would be a double-free. */
    terminal_destroy(s_term);
    s_term = NULL;
    ui_shell_show_home();
    bsp_display_unlock();

    vTaskDelete(NULL);
}

/* Shared by every transport: builds the status bar + terminal widget,
 * loads the screen, and starts the io task. Caller must already be holding
 * bsp_display_lock() (true both for the LVGL-click-handler path used by
 * Telnet, which runs inside the port's own lock, and for the background
 * connect tasks below, which take it explicitly). */
static void open_session_screen(const char *host_name, const char *protocol_label,
                                 session_transport_t transport)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    lv_obj_t *status_bar = lv_obj_create(scr);
    lv_obj_set_size(status_bar, LV_PCT(100), SESSION_STATUS_BAR_H);
    lv_obj_set_pos(status_bar, 0, 0);
    lv_obj_set_style_bg_color(status_bar, lv_color_hex(0x202020), 0);
    lv_obj_set_style_bg_opa(status_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(status_bar, 0, 0);
    lv_obj_set_style_pad_hor(status_bar, 8, 0);
    lv_obj_set_style_pad_ver(status_bar, 0, 0);
    lv_obj_set_style_pad_gap(status_bar, 8, 0);
    lv_obj_set_flex_flow(status_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    strncpy(s_session_host_name, host_name, sizeof(s_session_host_name) - 1);
    strncpy(s_session_protocol_label, protocol_label, sizeof(s_session_protocol_label) - 1);

    s_info_lbl = lv_label_create(status_bar);
    lv_obj_set_style_text_color(s_info_lbl, lv_color_white(), 0);
    update_session_info_label();

    lv_obj_t *btn_row = lv_obj_create(status_bar);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_set_style_pad_gap(btn_row, 8, 0);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_size(btn_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);

    s_jump_live_btn = lv_button_create(btn_row);
    lv_obj_t *jump_live_lbl = lv_label_create(s_jump_live_btn);
    lv_label_set_text(jump_live_lbl, "\xE2\x86\x93 Live"); /* down-arrow */
    lv_obj_add_event_cb(s_jump_live_btn, jump_to_live_btn_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_jump_live_btn, LV_OBJ_FLAG_HIDDEN); /* only shown once scrolled back */

    lv_obj_t *echo_btn = lv_button_create(btn_row);
    s_echo_btn_lbl = lv_label_create(echo_btn);
    lv_obj_add_event_cb(echo_btn, echo_toggle_btn_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn = lv_button_create(btn_row);
    lv_obj_t *btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, "Disconnect");
    lv_obj_add_event_cb(btn, disconnect_btn_click_cb, LV_EVENT_CLICKED, NULL);

    terminal_t *term = terminal_create(scr);
    lv_obj_t *canvas = terminal_get_widget(term);
    lv_obj_set_pos(canvas, 0, SESSION_STATUS_BAR_H);
    /* This screen has no scrollable content of its own -- disable the
     * default scrollable behavior so touch-dragging the canvas (our own
     * scrollback gesture, below) doesn't also fight the screen/canvas over
     * the gesture. */
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(canvas, LV_OBJ_FLAG_SCROLLABLE);
    /* lv_canvas is built on the image class, which explicitly clears
     * CLICKABLE on creation (a plain image isn't normally interactive) --
     * without re-adding it, the indev never registers touches on it at
     * all, so no press/pressing events (our scroll gesture) ever fire. */
    lv_obj_add_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(canvas, canvas_pressing_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(canvas, canvas_released_cb, LV_EVENT_RELEASED, NULL);
    terminal_render(term); /* the canvas pixel buffer starts uninitialized -- draw the
                             * blank grid now so the first frame isn't raw PSRAM garbage */

    s_term = term;
    s_transport = transport;
    s_stop_requested = false;
    s_local_echo = false;
    s_drag_pan_px = 0;
    s_last_scroll_render_us = 0;
    update_echo_btn_label();

    lv_screen_load_anim(scr, LV_SCREEN_LOAD_ANIM_NONE, 0, 0, true);
    /* Deliberately no lv_group/lv_indev_set_group here -- the raw keyboard
     * callback bypasses LVGL's focus/group input path entirely, so every
     * physical keypress (Tab included) goes straight to the remote session
     * instead of being interpreted as UI navigation. */
    keyboard_indev_set_raw_cb(raw_key_cb);

    xTaskCreate(session_io_task, "session_io", 8192, NULL, 5, NULL);
}

void session_ui_open_telnet(const host_entry_t *host)
{
    telnet_client_t *tc = telnet_client_connect(host->address, host->port, 8000);
    if (tc == NULL) {
        ESP_LOGE(TAG, "failed to connect to '%s' (%s:%u)", host->name, host->address, host->port);
        return; /* stay on the home screen -- caller is already showing it */
    }
    telnet_client_send_naws(tc, TERM_COLS, TERM_ROWS);
    open_session_screen(host->name, "telnet", telnet_client_as_transport(tc));
}

/* ---- SSH: password prompt, then connect in the background -------------- */

typedef struct {
    char name[HOST_NAME_MAX_LEN];
    char address[HOST_ADDRESS_MAX_LEN];
    uint16_t port;
    char username[HOST_USERNAME_MAX_LEN];
    char password[64];
} ssh_connect_args_t;

static char s_ssh_name[HOST_NAME_MAX_LEN];
static char s_ssh_address[HOST_ADDRESS_MAX_LEN];
static uint16_t s_ssh_port;
static char s_ssh_username[HOST_USERNAME_MAX_LEN];
static lv_obj_t *s_ssh_pw_ta;
static lv_obj_t *s_ssh_status_lbl;

static void show_ssh_password_screen(void);

static void ssh_connect_task(void *arg)
{
    ssh_connect_args_t *args = (ssh_connect_args_t *)arg;

    ssh_session_t *ssh = ssh_session_connect(args->address, args->port, args->username,
                                              args->password, TERM_COLS, TERM_ROWS);

    bsp_display_lock(0);
    if (ssh != NULL) {
        open_session_screen(args->name, "ssh", ssh_session_as_transport(ssh));
    } else if (lv_obj_is_valid(s_ssh_status_lbl)) {
        /* Validity check, not just non-NULL -- the user may have navigated
         * away from the password screen (it's since been auto-deleted)
         * while this connect attempt was still in flight. */
        lv_label_set_text(s_ssh_status_lbl, "Connect failed -- check username/password and try again");
    }
    bsp_display_unlock();

    free(args);
    vTaskDelete(NULL);
}

static void ssh_connect_btn_click_cb(lv_event_t *e)
{
    ssh_connect_args_t *args = malloc(sizeof(*args));
    if (args == NULL) {
        return;
    }
    memset(args, 0, sizeof(*args));
    strncpy(args->name, s_ssh_name, sizeof(args->name) - 1);
    strncpy(args->address, s_ssh_address, sizeof(args->address) - 1);
    args->port = s_ssh_port;
    strncpy(args->username, s_ssh_username, sizeof(args->username) - 1);
    strncpy(args->password, lv_textarea_get_text(s_ssh_pw_ta), sizeof(args->password) - 1);

    lv_label_set_text_fmt(s_ssh_status_lbl, "Connecting to '%s'...", s_ssh_name);
    xTaskCreate(ssh_connect_task, "ssh_connect", 8192, args, tskIDLE_PRIORITY + 1, NULL);
}

static void ssh_cancel_btn_click_cb(lv_event_t *e)
{
    ui_shell_show_home();
}

static void show_ssh_password_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, 18, 0);
    lv_obj_set_style_pad_gap(scr, 12, 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text_fmt(title, "SSH login for %s@%s", s_ssh_username, s_ssh_name);

    lv_obj_t *pw_row = lv_obj_create(scr);
    lv_obj_set_size(pw_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(pw_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(pw_row, 0, 0);
    lv_obj_set_style_pad_gap(pw_row, 12, 0);

    lv_obj_t *pw_label = lv_label_create(pw_row);
    lv_label_set_text(pw_label, "Password");
    lv_obj_set_width(pw_label, 165);

    s_ssh_pw_ta = lv_textarea_create(pw_row);
    lv_textarea_set_one_line(s_ssh_pw_ta, true);
    lv_textarea_set_password_mode(s_ssh_pw_ta, true);
    lv_obj_set_flex_grow(s_ssh_pw_ta, 1);

    s_ssh_status_lbl = lv_label_create(scr);
    lv_label_set_text(s_ssh_status_lbl, "");

    lv_obj_t *btn_row = lv_obj_create(scr);
    lv_obj_set_size(btn_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_set_style_pad_gap(btn_row, 12, 0);

    lv_obj_t *connect_btn = lv_button_create(btn_row);
    lv_obj_t *connect_lbl = lv_label_create(connect_btn);
    lv_label_set_text(connect_lbl, "Connect");
    lv_obj_add_event_cb(connect_btn, ssh_connect_btn_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cancel_btn = lv_button_create(btn_row);
    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_add_event_cb(cancel_btn, ssh_cancel_btn_click_cb, LV_EVENT_CLICKED, NULL);

    lv_group_t *group = lv_group_create();
    lv_group_add_obj(group, s_ssh_pw_ta);
    lv_group_add_obj(group, connect_btn);
    lv_group_add_obj(group, cancel_btn);

    lv_screen_load_anim(scr, LV_SCREEN_LOAD_ANIM_NONE, 0, 0, true);
    lv_indev_set_group(ui_shell_get_kbd_indev(), group);
}

void session_ui_open_ssh(const host_entry_t *host)
{
    if (host->auth_type == HOST_AUTH_KEY) {
        ESP_LOGE(TAG, "key-based auth isn't implemented yet -- use password auth for now");
        return;
    }

    strncpy(s_ssh_name, host->name, sizeof(s_ssh_name) - 1);
    strncpy(s_ssh_address, host->address, sizeof(s_ssh_address) - 1);
    s_ssh_port = host->port;
    strncpy(s_ssh_username, host->username, sizeof(s_ssh_username) - 1);

    show_ssh_password_screen();
}

void session_ui_init(void)
{
    s_key_queue = xQueueCreate(32, sizeof(session_key_event_t));
}
