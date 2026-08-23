#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "session_transport.h"

/*
 * Raw-socket Telnet client (Phase 4) -- no library risk here, this is what
 * validates the terminal widget independently of SSH's crypto concerns.
 * Handles IAC option negotiation (accepts server ECHO/SUPPRESS-GO-AHEAD,
 * offers TERMINAL-TYPE "xterm" and NAWS window size) inline on the read
 * path, since negotiation bytes can arrive interleaved with real data at
 * any point in the session, not just at connect time.
 */

typedef struct telnet_client telnet_client_t;

telnet_client_t *telnet_client_connect(const char *host, uint16_t port, uint32_t timeout_ms);

/* Reads already-IAC-stripped session bytes, blocking up to timeout_ms for
 * data to arrive. Returns >0 = bytes read, 0 = timed out with nothing
 * available, <0 = connection closed or errored. */
int telnet_client_read(telnet_client_t *tc, uint8_t *buf, size_t max_len, uint32_t timeout_ms);

/* Sends raw session bytes, IAC-byte-stuffing any literal 0xFF automatically. */
bool telnet_client_write(telnet_client_t *tc, const uint8_t *data, size_t len);

/* Best-effort: reports our terminal size once/if NAWS gets negotiated. */
void telnet_client_send_naws(telnet_client_t *tc, uint16_t cols, uint16_t rows);

void telnet_client_close(telnet_client_t *tc);

session_transport_t telnet_client_as_transport(telnet_client_t *tc);
