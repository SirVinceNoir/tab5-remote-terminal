#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "bsp/esp-bsp.h"
#include "wifi_manager.h"
#include "wifi_cp_ota.h"
#include "wifi_creds.h"

static const char *TAG = "wifi_manager";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define MAX_RETRY          5

static bool s_inited;
static EventGroupHandle_t s_event_group;
static SemaphoreHandle_t s_op_mutex; /* serializes scan/connect -- both touch the WiFi driver */
static int s_retry_num;
static bool s_connecting;
static bool s_is_connected;

static void event_handler(void *arg, esp_event_base_t event_base,
                           int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_is_connected = false;
        if (s_connecting && s_retry_num < MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGW(TAG, "retrying connection to AP (%d/%d)", s_retry_num, MAX_RETRY);
        } else if (s_connecting) {
            xEventGroupSetBits(s_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        s_is_connected = true;
        xEventGroupSetBits(s_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_manager_init(void)
{
    if (s_inited) {
        return;
    }

    ESP_LOGI(TAG, "enabling WiFi co-processor power");
    esp_err_t ret = bsp_feature_enable(BSP_FEATURE_WIFI, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bsp_feature_enable(WIFI) failed: %s", esp_err_to_name(ret));
    }
    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init() failed: %s (co-processor bring-up failed)",
                  esp_err_to_name(ret));
        return;
    }

    wifi_cp_ota_update_if_needed(); /* no-ops or restarts the host -- see wifi_cp_ota.c */

    s_event_group = xEventGroupCreate();
    s_op_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_inited = true;
}

int wifi_manager_scan(wifi_ap_record_t *out, int max_results)
{
    if (!s_inited || out == NULL || max_results <= 0) {
        return 0;
    }

    xSemaphoreTake(s_op_mutex, portMAX_DELAY);

    wifi_scan_config_t scan_cfg = { .show_hidden = false };
    esp_err_t ret = esp_wifi_scan_start(&scan_cfg, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_start() failed: %s", esp_err_to_name(ret));
        xSemaphoreGive(s_op_mutex);
        return 0;
    }

    uint16_t num = (uint16_t)max_results;
    ret = esp_wifi_scan_get_ap_records(&num, out);
    xSemaphoreGive(s_op_mutex);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_scan_get_ap_records() failed: %s", esp_err_to_name(ret));
        return 0;
    }
    return (int)num;
}

bool wifi_manager_connect(const char *ssid, const char *password, uint32_t timeout_ms)
{
    if (!s_inited || ssid == NULL || ssid[0] == '\0') {
        return false;
    }

    xSemaphoreTake(s_op_mutex, portMAX_DELAY);

    wifi_config_t wifi_config = { 0 };
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (password != NULL && password[0] != '\0') {
        strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    xEventGroupClearBits(s_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_retry_num = 0;
    s_connecting = true;

    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "connecting to SSID '%s'...", ssid);
        ret = esp_wifi_connect();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to start connecting to '%s': %s", ssid, esp_err_to_name(ret));
        s_connecting = false;
        xSemaphoreGive(s_op_mutex);
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(s_event_group,
                                            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                            pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    s_connecting = false;
    xSemaphoreGive(s_op_mutex);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to '%s'", ssid);
        wifi_creds_save(ssid, password != NULL ? password : "");
        return true;
    }

    ESP_LOGW(TAG, "failed to connect to '%s'", ssid);
    return false;
}

bool wifi_manager_connect_saved(uint32_t timeout_ms)
{
    char ssid[WIFI_CREDS_SSID_MAX_LEN + 1] = { 0 };
    char password[WIFI_CREDS_PASSWORD_MAX_LEN + 1] = { 0 };

    if (wifi_creds_load(ssid, sizeof(ssid), password, sizeof(password)) != ESP_OK) {
        ESP_LOGI(TAG, "no saved WiFi credentials -- use the WiFi screen to connect");
        return false;
    }

    ESP_LOGI(TAG, "attempting saved SSID '%s'", ssid);
    return wifi_manager_connect(ssid, password, timeout_ms);
}

bool wifi_manager_is_connected(void)
{
    return s_is_connected;
}
