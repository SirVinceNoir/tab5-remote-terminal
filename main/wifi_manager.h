#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_wifi_types.h"

/*
 * Real WiFi manager (Phase 3), superseding the Phase 2 hardcoded-SSID spike.
 * Brings up the ESP32-C6 co-processor and the STA driver once; scanning and
 * connecting are separate steps so a UI can drive them independently.
 */

#define WIFI_MANAGER_MAX_SCAN_RESULTS 32

/* Powers the C6, pushes coprocessor OTA if needed, starts STA mode. Safe to
 * call once at boot; does not attempt to connect. */
void wifi_manager_init(void);

/* Blocking scan; returns the number of APs written into out (<= max_results). */
int wifi_manager_scan(wifi_ap_record_t *out, int max_results);

/* Blocking connect attempt. On success, saves (ssid, password) as the new
 * "last known good" credentials via wifi_creds_save(). */
bool wifi_manager_connect(const char *ssid, const char *password, uint32_t timeout_ms);

/* Loads the saved credentials (if any) and attempts to connect. Returns
 * false immediately if nothing has been saved yet. */
bool wifi_manager_connect_saved(uint32_t timeout_ms);

bool wifi_manager_is_connected(void);
