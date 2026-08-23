#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#define WIFI_CREDS_MAX_NETWORKS     8
#define WIFI_CREDS_SSID_MAX_LEN     32
#define WIFI_CREDS_PASSWORD_MAX_LEN 64

typedef struct {
    char ssid[WIFI_CREDS_SSID_MAX_LEN + 1];
    char password[WIFI_CREDS_PASSWORD_MAX_LEN + 1];
} wifi_network_t;

/*
 * Small list of remembered networks, most-recently-used first, persisted as
 * JSON on the same LittleFS partition host_store.c uses -- call after
 * host_store_init() has mounted it. Replaces the original single-slot NVS
 * store; that old entry is not migrated (dev firmware, no existing users to
 * carry forward), so a previously-saved network needs re-entering once.
 */
void wifi_creds_init(void);

int wifi_creds_count(void);
const wifi_network_t *wifi_creds_get(int index); /* 0 = most recently used */
const wifi_network_t *wifi_creds_find(const char *ssid); /* NULL if not saved */

/* Adds a new network or updates an existing one's password (matched by
 * SSID), moves it to the front, persists to disk. Evicts the
 * least-recently-used entry if already at WIFI_CREDS_MAX_NETWORKS. */
esp_err_t wifi_creds_save(const char *ssid, const char *password);
