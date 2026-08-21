#pragma once

/*
 * Phase 2 spike: validate libssh2_esp actually works on ESP32-P4 (it was
 * only proven on ESP32-S3 upstream). Connects to a hardcoded test host,
 * authenticates by password, runs one command, and logs the output plus
 * an explicit PASS/FAIL. Throwaway code -- no persistence, no known_hosts
 * verification (that's for Phase 5's real SSH client).
 *
 * Call only after WiFi is confirmed connected.
 */
void ssh_spike_start(void);
