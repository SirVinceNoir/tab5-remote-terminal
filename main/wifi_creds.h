#pragma once

#include <stddef.h>
#include "esp_err.h"

#define WIFI_CREDS_SSID_MAX_LEN     32
#define WIFI_CREDS_PASSWORD_MAX_LEN 64

/*
 * NVS-backed storage for the single most-recently-connected WiFi network.
 * Returns ESP_ERR_NVS_NOT_FOUND if nothing has been saved yet.
 */
esp_err_t wifi_creds_load(char *ssid_out, size_t ssid_out_len,
                           char *password_out, size_t password_out_len);
esp_err_t wifi_creds_save(const char *ssid, const char *password);
