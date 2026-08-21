#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Phase 3: persistent host list storage. Hosts live in a single JSON file
 * on LittleFS ("storage" partition). This is real, load-bearing code (not
 * a throwaway spike) -- it's what the navigation UI and WiFi manager sit
 * on top of.
 *
 * Deliberately NOT stored here: passwords. auth_type records how a host
 * authenticates, but the secret itself (password, or a path to a key file)
 * is out of scope for this storage layer -- see Phase 5 notes on SSH key
 * management for where that lands.
 */

#define HOST_STORE_MAX_HOSTS   64
#define HOST_NAME_MAX_LEN      64
#define HOST_ADDRESS_MAX_LEN   64
#define HOST_USERNAME_MAX_LEN  32
#define HOST_GROUP_MAX_LEN     64

typedef enum {
    HOST_PROTOCOL_SSH = 0,
    HOST_PROTOCOL_TELNET,
    HOST_PROTOCOL_SERIAL,
} host_protocol_t;

typedef enum {
    HOST_AUTH_PASSWORD = 0,
    HOST_AUTH_KEY,
} host_auth_type_t;

typedef struct {
    char name[HOST_NAME_MAX_LEN];
    char group[HOST_GROUP_MAX_LEN]; /* empty string = ungrouped */
    host_protocol_t protocol;
    char address[HOST_ADDRESS_MAX_LEN]; /* hostname or IP; unused for HOST_PROTOCOL_SERIAL */
    uint16_t port;
    char username[HOST_USERNAME_MAX_LEN];
    host_auth_type_t auth_type;
    uint16_t keepalive_sec; /* 0 = disabled */
} host_entry_t;

/* Mounts LittleFS and loads the host list from flash (creates an empty
 * list if the file doesn't exist yet). Must be called once before any
 * other host_store_* function. */
esp_err_t host_store_init(void);

int host_store_count(void);

/* Returns NULL if index is out of range. Pointer is valid until the next
 * host_store_add/update/delete call. */
const host_entry_t *host_store_get(int index);

/* Adds a copy of *host to the list and persists to flash. */
esp_err_t host_store_add(const host_entry_t *host);

esp_err_t host_store_update(int index, const host_entry_t *host);

esp_err_t host_store_delete(int index);

#ifdef __cplusplus
}
#endif
