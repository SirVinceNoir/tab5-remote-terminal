#pragma once

#include <stdint.h>
#include "session_transport.h"

/*
 * Real interactive SSH client (Phase 5), built on libssh2_esp -- confirmed
 * working on this exact chip by the Phase 2 spike (ssh_spike.c), which this
 * reuses the non-blocking handshake/auth pattern from. Opens a PTY-backed
 * shell channel (not exec) sized to match the terminal widget, so it slots
 * into the same session screen/task as Telnet via session_transport_t.
 *
 * v1 scope: password auth only (host->auth_type == HOST_AUTH_KEY is
 * rejected by the caller for now -- key-based auth + on-device key
 * generation is the planned follow-up). Host-key verification is NOT
 * implemented -- the remote's fingerprint is logged but never checked
 * against a known_hosts store, so this offers no protection against a
 * MITM on the network path. Flagging that rather than leaving it quiet:
 * close this gap before pointing it at anything sensitive.
 */
typedef struct ssh_session ssh_session_t;

ssh_session_t *ssh_session_connect(const char *host, uint16_t port, const char *username,
                                    const char *password, int cols, int rows);

session_transport_t ssh_session_as_transport(ssh_session_t *s);
