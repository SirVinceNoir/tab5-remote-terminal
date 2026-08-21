#pragma once

#include <stdbool.h>

/*
 * Phase 2 spike: bring up WiFi over the ESP32-C6 co-processor (ESP-Hosted
 * transport via esp_wifi_remote) and connect to a hardcoded AP. Throwaway
 * code -- logs pass/fail to serial, no persistence, no UI. Superseded by the
 * real WiFi manager UI in Phase 3.
 *
 * Returns true if the connection succeeded (so callers can gate further
 * spikes that need network, e.g. the SSH spike, on this).
 */
bool wifi_spike_start(void);
