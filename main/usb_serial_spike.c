#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "bsp/esp-bsp.h"
#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"
#include "usb/vcp_ch34x.h"
#include "usb/vcp_cp210x.h"
#include "usb/vcp_ftdi.h"
#include "usb_serial_spike.h"

/*
 * Adapted from ESP-IDF's own examples/peripherals/usb/host/cdc reference,
 * trimmed for this project: single device (not a multi-slot table), no
 * quit-button GPIO, no line-coding/control-line poking -- just prove
 * enumeration + read/write against whatever's plugged into the USB-A port.
 * Reuses the BSP's bsp_usb_host_start() for the base USB Host stack + power
 * (it already calls usb_host_install() + pumps events) instead of
 * duplicating that; we only add the CDC-ACM class driver on top.
 */

#define TX_TEST_STRING     "Tab5 USB serial spike\r\n"
#define TX_TIMEOUT_MS      1000
#define ESPRESSIF_VID      0x303A

static const char *TAG = "usb_serial_spike";

static QueueHandle_t s_app_queue;
static cdc_acm_dev_hdl_t s_cdc_dev;

typedef struct {
    enum { APP_DEVICE_CONNECTED, APP_DEVICE_DISCONNECTED } id;
    uint16_t vid;
    uint16_t pid;
} app_message_t;

static bool handle_rx(const uint8_t *data, size_t data_len, void *arg)
{
    ESP_LOGI(TAG, "received %u bytes:", (unsigned int)data_len);
    ESP_LOG_BUFFER_HEXDUMP(TAG, data, data_len, ESP_LOG_INFO);
    return true;
}

static void handle_event(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    switch (event->type) {
    case CDC_ACM_HOST_ERROR:
        ESP_LOGE(TAG, "CDC-ACM error: %d", event->data.error);
        break;
    case CDC_ACM_HOST_DEVICE_DISCONNECTED: {
        app_message_t msg = { .id = APP_DEVICE_DISCONNECTED };
        xQueueSend(s_app_queue, &msg, 0);
        break;
    }
    case CDC_ACM_HOST_SERIAL_STATE:
        ESP_LOGI(TAG, "serial state notif 0x%04X", event->data.serial_state.val);
        break;
    default:
        break;
    }
}

static void new_dev_cb(usb_device_handle_t usb_dev)
{
    const usb_device_desc_t *device_desc;
    if (usb_host_get_device_descriptor(usb_dev, &device_desc) != ESP_OK) {
        return;
    }
    app_message_t msg = {
        .id = APP_DEVICE_CONNECTED,
        .vid = device_desc->idVendor,
        .pid = device_desc->idProduct,
    };
    xQueueSend(s_app_queue, &msg, 0);
}

static cdc_acm_dev_hdl_t open_cdc_device(uint16_t vid, uint16_t pid,
                                         const cdc_acm_host_device_config_t *dev_config)
{
    cdc_acm_dev_hdl_t dev = NULL;
    esp_err_t err;

    switch (vid) {
    case FTDI_VID:
        err = ftdi_vcp_open(pid, 0, dev_config, &dev);
        break;
    case NANJING_QINHENG_MICROE_VID:
        err = ch34x_vcp_open(pid, 0, dev_config, &dev);
        break;
    case SILICON_LABS_VID:
        err = cp210x_vcp_open(pid, 0, dev_config, &dev);
        break;
    case ESPRESSIF_VID:
    default:
        err = cdc_acm_host_open(vid, pid, 0, dev_config, &dev);
        break;
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB SPIKE: FAIL -- could not open device VID=0x%04X PID=0x%04X (%s)",
                  vid, pid, esp_err_to_name(err));
        return NULL;
    }
    return dev;
}

static void usb_serial_task(void *arg)
{
    s_app_queue = xQueueCreate(10, sizeof(app_message_t));

    esp_err_t ret = bsp_usb_host_start(BSP_USB_HOST_POWER_MODE_USB_DEV, false);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "USB SPIKE: FAIL -- bsp_usb_host_start() returned %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }

    const cdc_acm_host_driver_config_t driver_config = {
        .driver_task_stack_size = 4096,
        .driver_task_priority = 10,
        .xCoreID = 0,
        .new_dev_cb = new_dev_cb,
    };
    ret = cdc_acm_host_install(&driver_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "USB SPIKE: FAIL -- cdc_acm_host_install() returned %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "waiting for a USB-serial adapter on the USB-A host port...");

    cdc_acm_host_device_config_t dev_config = {
        .connection_timeout_ms = 0,
        .out_buffer_size = 512,
        .in_buffer_size = 512,
        .user_arg = NULL,
        .event_cb = handle_event,
        .data_cb = handle_rx,
    };

    while (1) {
        app_message_t msg;
        xQueueReceive(s_app_queue, &msg, portMAX_DELAY);

        if (msg.id == APP_DEVICE_CONNECTED) {
            ESP_LOGI(TAG, "device connected VID=0x%04X PID=0x%04X", msg.vid, msg.pid);
            s_cdc_dev = open_cdc_device(msg.vid, msg.pid, &dev_config);
            if (s_cdc_dev == NULL) {
                continue;
            }

            ESP_LOGI(TAG, "device opened -- descriptor:");
            cdc_acm_host_desc_print(s_cdc_dev);

            ret = cdc_acm_host_data_tx_blocking(s_cdc_dev, (const uint8_t *)TX_TEST_STRING,
                                                 strlen(TX_TEST_STRING), TX_TIMEOUT_MS);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "USB SPIKE: PASS -- enumerated, opened, and wrote %u bytes "
                              "(any received data logged above)", (unsigned int)strlen(TX_TEST_STRING));
            } else {
                ESP_LOGE(TAG, "USB SPIKE: FAIL -- data_tx_blocking() returned %s", esp_err_to_name(ret));
            }
        } else if (msg.id == APP_DEVICE_DISCONNECTED) {
            ESP_LOGW(TAG, "device disconnected");
            if (s_cdc_dev != NULL) {
                cdc_acm_host_close(s_cdc_dev);
                s_cdc_dev = NULL;
            }
            ESP_LOGI(TAG, "waiting for a USB-serial adapter on the USB-A host port...");
        }
    }
}

void usb_serial_spike_start(void)
{
    xTaskCreate(usb_serial_task, "usb_serial_spike", 4096, NULL, 10, NULL);
}
