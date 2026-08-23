#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "bsp/esp-bsp.h"
#include "wifi_manager.h"
#include "wifi_cp_ota.h"
#include "wifi_creds.h"

static const char *TAG = "wifi_manager";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define MAX_RETRY          5
#define RECONNECT_DELAY_US (5ULL * 1000 * 1000) /* between spontaneous-drop retries */

static bool s_inited;
static EventGroupHandle_t s_event_group;
static SemaphoreHandle_t s_op_mutex; /* serializes scan/connect -- both touch the WiFi driver */
static int s_retry_num;
static bool s_connecting;
static bool s_is_connected;
static esp_timer_handle_t s_reconnect_timer;

static void reconnect_timer_cb(void *arg)
{
    ESP_LOGI(TAG, "attempting to reconnect...");
    esp_wifi_connect();
}

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
        } else {
            /* Spontaneous drop well after we were already up and running --
             * out of range, an AP hiccup, interference. Without this, any
             * such drop was permanent until the user manually reconnected
             * via the WiFi screen, which read as "WiFi is unreliable" when
             * really nothing was ever retrying. Keep trying at a fixed
             * interval (not a tight loop) since this is a personal network
             * that isn't going anywhere. */
            ESP_LOGW(TAG, "WiFi connection dropped -- will retry in %llu s",
                      RECONNECT_DELAY_US / 1000000ULL);
            esp_timer_stop(s_reconnect_timer); /* no-op if not already running */
            esp_timer_start_once(s_reconnect_timer, RECONNECT_DELAY_US);
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

    wifi_creds_init();

    s_event_group = xEventGroupCreate();
    s_op_mutex = xSemaphoreCreateMutex();

    const esp_timer_create_args_t reconnect_timer_args = {
        .callback = reconnect_timer_cb,
        .name = "wifi_reconnect",
    };
    ESP_ERROR_CHECK(esp_timer_create(&reconnect_timer_args, &s_reconnect_timer));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Every disconnect logged so far has been reason=200 (BEACON_TIMEOUT) at
     * decent signal strength (-57 to -63 dBm) -- not a weak-signal pattern.
     * Modem sleep's periodic doze/wake cycle is a well-known cause of missed
     * beacons on ESP32 in general, and doubly a risk here: the radio's
     * sleep/wake scheduling lives on the C6 co-processor while we coordinate
     * with it over an SDIO RPC bridge, an extra hop of latency/jitter a
     * normal single-chip WiFi setup wouldn't have. Disabling power save
     * trades a bit of extra draw for the radio never missing a beacon. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

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
    const wifi_network_t *net = wifi_creds_get(0); /* most recently used */
    if (net == NULL) {
        ESP_LOGI(TAG, "no saved WiFi credentials -- use the WiFi screen to connect");
        return false;
    }

    ESP_LOGI(TAG, "attempting saved SSID '%s'", net->ssid);
    return wifi_manager_connect(net->ssid, net->password, timeout_ms);
}

bool wifi_manager_is_connected(void)
{
    return s_is_connected;
}

bool wifi_manager_get_status(char *ssid_out, size_t ssid_out_len)
{
    if (!s_is_connected) {
        return false;
    }
    wifi_ap_record_t info;
    if (esp_wifi_sta_get_ap_info(&info) != ESP_OK) {
        return false;
    }
    if (ssid_out != NULL && ssid_out_len > 0) {
        strncpy(ssid_out, (const char *)info.ssid, ssid_out_len - 1);
        ssid_out[ssid_out_len - 1] = '\0';
    }
    return true;
}
