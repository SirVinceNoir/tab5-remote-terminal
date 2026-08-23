#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "cJSON.h"
#include "wifi_creds.h"

static const char *TAG = "wifi_creds";

#define MOUNT_POINT       "/storage"
#define NETWORKS_FILE     MOUNT_POINT "/wifi_networks.json"

static wifi_network_t s_networks[WIFI_CREDS_MAX_NETWORKS];
static int s_count;

static esp_err_t persist_to_disk(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "networks");

    for (int i = 0; i < s_count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", s_networks[i].ssid);
        cJSON_AddStringToObject(item, "password", s_networks[i].password);
        cJSON_AddItemToArray(arr, item);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json_str == NULL) {
        ESP_LOGE(TAG, "cJSON_PrintUnformatted() failed");
        return ESP_ERR_NO_MEM;
    }

    FILE *f = fopen(NETWORKS_FILE, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "failed to open '%s' for writing", NETWORKS_FILE);
        cJSON_free(json_str);
        return ESP_FAIL;
    }
    fputs(json_str, f);
    fclose(f);
    cJSON_free(json_str);

    ESP_LOGI(TAG, "saved %d network(s) to %s", s_count, NETWORKS_FILE);
    return ESP_OK;
}

void wifi_creds_init(void)
{
    s_count = 0;

    FILE *f = fopen(NETWORKS_FILE, "r");
    if (f == NULL) {
        ESP_LOGI(TAG, "'%s' not found -- no saved networks yet", NETWORKS_FILE);
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
        ESP_LOGE(TAG, "out of memory reading '%s' (%ld bytes)", NETWORKS_FILE, size);
        fclose(f);
        return;
    }
    size_t read = fread(buf, 1, size, f);
    buf[read] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL) {
        ESP_LOGE(TAG, "'%s' contains invalid JSON -- starting with no saved networks", NETWORKS_FILE);
        return;
    }

    cJSON *arr = cJSON_GetObjectItem(root, "networks");
    if (cJSON_IsArray(arr)) {
        cJSON *item;
        cJSON_ArrayForEach(item, arr) {
            if (s_count >= WIFI_CREDS_MAX_NETWORKS) {
                break;
            }
            cJSON *ssid = cJSON_GetObjectItem(item, "ssid");
            cJSON *password = cJSON_GetObjectItem(item, "password");
            if (!cJSON_IsString(ssid)) {
                continue;
            }
            wifi_network_t *net = &s_networks[s_count];
            memset(net, 0, sizeof(*net));
            strncpy(net->ssid, ssid->valuestring, sizeof(net->ssid) - 1);
            if (cJSON_IsString(password)) {
                strncpy(net->password, password->valuestring, sizeof(net->password) - 1);
            }
            s_count++;
        }
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "loaded %d saved network(s) from %s", s_count, NETWORKS_FILE);
}

int wifi_creds_count(void)
{
    return s_count;
}

const wifi_network_t *wifi_creds_get(int index)
{
    if (index < 0 || index >= s_count) {
        return NULL;
    }
    return &s_networks[index];
}

const wifi_network_t *wifi_creds_find(const char *ssid)
{
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_networks[i].ssid, ssid) == 0) {
            return &s_networks[i];
        }
    }
    return NULL;
}

esp_err_t wifi_creds_save(const char *ssid, const char *password)
{
    wifi_network_t entry = { 0 };
    strncpy(entry.ssid, ssid, sizeof(entry.ssid) - 1);
    strncpy(entry.password, password != NULL ? password : "", sizeof(entry.password) - 1);

    int existing = -1;
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_networks[i].ssid, ssid) == 0) {
            existing = i;
            break;
        }
    }
    if (existing >= 0) {
        for (int i = existing; i < s_count - 1; i++) {
            s_networks[i] = s_networks[i + 1];
        }
        s_count--;
    }

    int insert_count = (s_count < WIFI_CREDS_MAX_NETWORKS) ? s_count : WIFI_CREDS_MAX_NETWORKS - 1;
    for (int i = insert_count; i > 0; i--) {
        s_networks[i] = s_networks[i - 1];
    }
    s_networks[0] = entry;
    s_count = insert_count + 1;

    ESP_LOGI(TAG, "saved WiFi credentials for SSID '%s'", ssid);
    return persist_to_disk();
}
