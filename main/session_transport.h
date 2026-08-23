#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * Minimal transport abstraction so session_ui.c's terminal-session screen
 * doesn't care whether it's driving a Telnet socket or an SSH channel.
 *
 * read() semantics match telnet_client_read()/the SSH channel wrapper:
 * >0 = bytes read into buf, 0 = timed out with nothing available (not an
 * error -- keep looping), <0 = the session is over (closed or errored).
 */
typedef struct {
    void *ctx;
    int (*read)(void *ctx, uint8_t *buf, size_t max_len, uint32_t timeout_ms);
    bool (*write)(void *ctx, const uint8_t *data, size_t len);
    void (*close)(void *ctx);
} session_transport_t;
