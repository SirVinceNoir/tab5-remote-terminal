#include "esp_log.h"
#include "nvs.h"
#include "wifi_creds.h"

static const char *TAG = "wifi_creds";
#define NVS_NAMESPACE "wifi_creds"

esp_err_t wifi_creds_load(char *ssid_out, size_t ssid_out_len,
                           char *password_out, size_t password_out_len)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (ret != ESP_OK) {
        return ret; /* namespace not created yet -- nothing saved */
    }

    size_t len = ssid_out_len;
    ret = nvs_get_str(h, "ssid", ssid_out, &len);
    if (ret != ESP_OK) {
        nvs_close(h);
        return ret;
    }

    len = password_out_len;
    ret = nvs_get_str(h, "pass", password_out, &len);
    nvs_close(h);
    return ret;
}

esp_err_t wifi_creds_save(const char *ssid, const char *password)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open() failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_set_str(h, "ssid", ssid);
    if (ret == ESP_OK) {
        ret = nvs_set_str(h, "pass", password != NULL ? password : "");
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(h);
    }
    nvs_close(h);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to save WiFi credentials: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "saved WiFi credentials for SSID '%s'", ssid);
    }
    return ret;
}
