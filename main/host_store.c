#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_littlefs.h"
#include "cJSON.h"
#include "host_store.h"

static const char *TAG = "host_store";

#define MOUNT_POINT   "/storage"
#define HOSTS_FILE    MOUNT_POINT "/hosts.json"

static host_entry_t s_hosts[HOST_STORE_MAX_HOSTS];
static int s_host_count;

static const char *protocol_to_str(host_protocol_t p)
{
    switch (p) {
    case HOST_PROTOCOL_SSH:    return "ssh";
    case HOST_PROTOCOL_TELNET: return "telnet";
    case HOST_PROTOCOL_SERIAL: return "serial";
    default:                   return "ssh";
    }
}

static host_protocol_t protocol_from_str(const char *s)
{
    if (s != NULL) {
        if (strcmp(s, "telnet") == 0) {
            return HOST_PROTOCOL_TELNET;
        }
        if (strcmp(s, "serial") == 0) {
            return HOST_PROTOCOL_SERIAL;
        }
    }
    return HOST_PROTOCOL_SSH;
}

static const char *auth_type_to_str(host_auth_type_t a)
{
    return (a == HOST_AUTH_KEY) ? "key" : "password";
}

static host_auth_type_t auth_type_from_str(const char *s)
{
    return (s != NULL && strcmp(s, "key") == 0) ? HOST_AUTH_KEY : HOST_AUTH_PASSWORD;
}

static esp_err_t persist_to_disk(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *hosts_arr = cJSON_AddArrayToObject(root, "hosts");

    for (int i = 0; i < s_host_count; i++) {
        const host_entry_t *h = &s_hosts[i];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", h->name);
        cJSON_AddStringToObject(item, "group", h->group);
        cJSON_AddStringToObject(item, "protocol", protocol_to_str(h->protocol));
        cJSON_AddStringToObject(item, "address", h->address);
        cJSON_AddNumberToObject(item, "port", h->port);
        cJSON_AddStringToObject(item, "username", h->username);
        cJSON_AddStringToObject(item, "auth_type", auth_type_to_str(h->auth_type));
        cJSON_AddNumberToObject(item, "keepalive_sec", h->keepalive_sec);
        cJSON_AddItemToArray(hosts_arr, item);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json_str == NULL) {
        ESP_LOGE(TAG, "cJSON_PrintUnformatted() failed");
        return ESP_ERR_NO_MEM;
    }

    FILE *f = fopen(HOSTS_FILE, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "failed to open '%s' for writing", HOSTS_FILE);
        cJSON_free(json_str);
        return ESP_FAIL;
    }
    fputs(json_str, f);
    fclose(f);
    cJSON_free(json_str);

    ESP_LOGI(TAG, "saved %d host(s) to %s", s_host_count, HOSTS_FILE);
    return ESP_OK;
}

static void load_from_disk(void)
{
    s_host_count = 0;

    FILE *f = fopen(HOSTS_FILE, "r");
    if (f == NULL) {
        ESP_LOGI(TAG, "'%s' not found -- starting with an empty host list", HOSTS_FILE);
        return;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        return;
    }

    char *buf = malloc(size + 1);
    if (buf == NULL) {
        ESP_LOGE(TAG, "out of memory reading '%s' (%ld bytes)", HOSTS_FILE, size);
        fclose(f);
        return;
    }
    size_t read = fread(buf, 1, size, f);
    buf[read] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL) {
        ESP_LOGE(TAG, "'%s' contains invalid JSON -- starting with an empty host list", HOSTS_FILE);
        return;
    }

    cJSON *hosts_arr = cJSON_GetObjectItem(root, "hosts");
    if (cJSON_IsArray(hosts_arr)) {
        cJSON *item;
        cJSON_ArrayForEach(item, hosts_arr) {
            if (s_host_count >= HOST_STORE_MAX_HOSTS) {
                ESP_LOGW(TAG, "hit HOST_STORE_MAX_HOSTS (%d) -- ignoring the rest", HOST_STORE_MAX_HOSTS);
                break;
            }
            host_entry_t *h = &s_hosts[s_host_count];
            memset(h, 0, sizeof(*h));

            cJSON *v;
            if ((v = cJSON_GetObjectItem(item, "name")) && cJSON_IsString(v)) {
                strncpy(h->name, v->valuestring, sizeof(h->name) - 1);
            }
            if ((v = cJSON_GetObjectItem(item, "group")) && cJSON_IsString(v)) {
                strncpy(h->group, v->valuestring, sizeof(h->group) - 1);
            }
            if ((v = cJSON_GetObjectItem(item, "protocol")) && cJSON_IsString(v)) {
                h->protocol = protocol_from_str(v->valuestring);
            }
            if ((v = cJSON_GetObjectItem(item, "address")) && cJSON_IsString(v)) {
                strncpy(h->address, v->valuestring, sizeof(h->address) - 1);
            }
            if ((v = cJSON_GetObjectItem(item, "port")) && cJSON_IsNumber(v)) {
                h->port = (uint16_t)v->valueint;
            }
            if ((v = cJSON_GetObjectItem(item, "username")) && cJSON_IsString(v)) {
                strncpy(h->username, v->valuestring, sizeof(h->username) - 1);
            }
            if ((v = cJSON_GetObjectItem(item, "auth_type")) && cJSON_IsString(v)) {
                h->auth_type = auth_type_from_str(v->valuestring);
            }
            if ((v = cJSON_GetObjectItem(item, "keepalive_sec")) && cJSON_IsNumber(v)) {
                h->keepalive_sec = (uint16_t)v->valueint;
            }

            s_host_count++;
        }
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "loaded %d host(s) from %s", s_host_count, HOSTS_FILE);
}

esp_err_t host_store_init(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = MOUNT_POINT,
        .partition_label = "storage",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_vfs_littlefs_register() failed: %s", esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0, used = 0;
    if (esp_littlefs_info(conf.partition_label, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "LittleFS mounted: %u/%u bytes used", (unsigned int)used, (unsigned int)total);
    }

    load_from_disk();
    return ESP_OK;
}

int host_store_count(void)
{
    return s_host_count;
}

const host_entry_t *host_store_get(int index)
{
    if (index < 0 || index >= s_host_count) {
        return NULL;
    }
    return &s_hosts[index];
}

esp_err_t host_store_add(const host_entry_t *host)
{
    if (s_host_count >= HOST_STORE_MAX_HOSTS) {
        ESP_LOGE(TAG, "host list is full (max %d)", HOST_STORE_MAX_HOSTS);
        return ESP_ERR_NO_MEM;
    }
    s_hosts[s_host_count] = *host;
    s_host_count++;
    return persist_to_disk();
}

esp_err_t host_store_update(int index, const host_entry_t *host)
{
    if (index < 0 || index >= s_host_count) {
        return ESP_ERR_INVALID_ARG;
    }
    s_hosts[index] = *host;
    return persist_to_disk();
}

esp_err_t host_store_delete(int index)
{
    if (index < 0 || index >= s_host_count) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = index; i < s_host_count - 1; i++) {
        s_hosts[i] = s_hosts[i + 1];
    }
    s_host_count--;
    return persist_to_disk();
}
