#pragma once

#include "lvgl.h"
#include "host_store.h"

/*
 * The live terminal session screen (Phase 4): connects, renders remote
 * output through the terminal widget, and forwards physical-keyboard input
 * to the session -- currently Telnet only, SSH/serial follow in Phase 5/6
 * reusing the same terminal widget.
 */
void session_ui_init(void);
void session_ui_open_telnet(const host_entry_t *host);
void session_ui_open_ssh(const host_entry_t *host);
