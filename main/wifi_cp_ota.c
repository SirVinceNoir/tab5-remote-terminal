#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_partition.h"
#include "esp_app_format.h"
#include "esp_hosted.h"
#include "esp_hosted_ota.h"
#include "esp_hosted_api_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_cp_ota.h"

static const char *TAG = "wifi_cp_ota";

#define SLAVE_FW_PARTITION_LABEL "slave_fw"
#define CHUNK_SIZE 1500

/* Adapted from espressif/esp_hosted's examples/ota/coprocessor_ota reference
 * (ota_partition.c + main.c's activate_and_restart()), trimmed to what this
 * project needs: no Kconfig knobs, always version-checked so re-running this
 * on every boot is a fast no-op once the coprocessor is up to date. */

static esp_err_t parse_image_header(const esp_partition_t *partition, size_t *firmware_size)
{
    esp_image_header_t image_header;
    esp_image_segment_header_t segment_header;
    esp_err_t ret;
    size_t offset;
    size_t total_size;

    ret = esp_partition_read(partition, 0, &image_header, sizeof(image_header));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to read image header: %s", esp_err_to_name(ret));
        return ret;
    }
    if (image_header.magic != ESP_IMAGE_HEADER_MAGIC) {
        ESP_LOGE(TAG, "partition '%s' does not contain a valid firmware image (magic 0x%02x)",
                 partition->label, image_header.magic);
        return ESP_ERR_INVALID_ARG;
    }

    offset = sizeof(image_header);
    total_size = sizeof(image_header);

    for (int i = 0; i < image_header.segment_count; i++) {
        ret = esp_partition_read(partition, offset, &segment_header, sizeof(segment_header));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "failed to read segment %d header: %s", i, esp_err_to_name(ret));
            return ret;
        }

        total_size += sizeof(segment_header) + segment_header.data_len;
        offset += sizeof(segment_header) + segment_header.data_len;
    }

    size_t padding = (16 - (total_size % 16)) % 16;
    total_size += padding + 1; /* alignment + checksum byte */

    if (image_header.hash_appended == 1) {
        size_t hash_padding = (16 - (total_size % 16)) % 16;
        total_size += hash_padding + 32; /* SHA256 */
    }

    *firmware_size = total_size;
    return ESP_OK;
}

static void restart_host(void)
{
    ESP_LOGW(TAG, "restarting host to resync with coprocessor...");
    esp_hosted_deinit();
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}

void wifi_cp_ota_update_if_needed(void)
{
    const esp_partition_t *partition;
    esp_err_t ret;
    size_t firmware_size;

    /*
     * Both this host and the ~/esp/tab5-c6-coprocessor build are pinned to
     * espressif/esp_hosted ^3.0.6 (see main/idf_component.yml and that
     * project's main/idf_component.yml). Compare the coprocessor's reported
     * ESP-Hosted protocol version against that pin -- NOT against
     * esp_app_desc_t.version, which is an unrelated per-project build string
     * ("1" for this trivial CP app) that will never match and would cause
     * this to re-push and restart on every single boot forever.
     */
    static const uint32_t expected_major = 3, expected_minor = 0, expected_patch = 6;

    partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
                                          SLAVE_FW_PARTITION_LABEL);
    if (partition == NULL) {
        ESP_LOGW(TAG, "no '%s' partition -- skipping coprocessor OTA", SLAVE_FW_PARTITION_LABEL);
        return;
    }

    ret = parse_image_header(partition, &firmware_size);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "'%s' partition has no valid firmware baked in -- skipping coprocessor OTA",
                 SLAVE_FW_PARTITION_LABEL);
        return;
    }
    if (firmware_size > partition->size) {
        ESP_LOGE(TAG, "baked-in firmware (%u bytes) exceeds partition size (%" PRIu32 ")",
                  (unsigned int)firmware_size, partition->size);
        return;
    }

    esp_hosted_coprocessor_fwver_t running_version = {0};
    if (esp_hosted_get_coprocessor_fwversion(&running_version) == ESP_OK) {
        ESP_LOGI(TAG, "coprocessor running esp-hosted %" PRIu32 ".%" PRIu32 ".%" PRIu32
                 " (expecting %" PRIu32 ".%" PRIu32 ".%" PRIu32 ")",
                 running_version.major1, running_version.minor1, running_version.patch1,
                 expected_major, expected_minor, expected_patch);

        if (running_version.major1 == expected_major &&
            running_version.minor1 == expected_minor &&
            running_version.patch1 == expected_patch) {
            ESP_LOGI(TAG, "coprocessor already up to date -- skipping OTA");
            return;
        }
    } else {
        ESP_LOGW(TAG, "could not read coprocessor firmware version -- proceeding with OTA anyway");
    }

    ESP_LOGI(TAG, "pushing %u bytes from '%s' to coprocessor...",
             (unsigned int)firmware_size, SLAVE_FW_PARTITION_LABEL);

    ret = esp_hosted_slave_ota_begin();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_hosted_slave_ota_begin() failed: %s", esp_err_to_name(ret));
        return;
    }

    uint8_t chunk[CHUNK_SIZE];
    size_t offset = 0;
    while (offset < firmware_size) {
        size_t bytes = (firmware_size - offset > CHUNK_SIZE) ? CHUNK_SIZE : (firmware_size - offset);
        ret = esp_partition_read(partition, offset, chunk, bytes);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "failed reading partition at offset %u: %s", (unsigned int)offset, esp_err_to_name(ret));
            esp_hosted_slave_ota_end();
            return;
        }
        ret = esp_hosted_slave_ota_write(chunk, bytes);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_hosted_slave_ota_write() failed at offset %u: %s",
                      (unsigned int)offset, esp_err_to_name(ret));
            esp_hosted_slave_ota_end();
            return;
        }
        offset += bytes;
    }

    ret = esp_hosted_slave_ota_end();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_hosted_slave_ota_end() failed: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "coprocessor OTA transfer complete, activating...");
    ret = esp_hosted_slave_ota_activate();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_hosted_slave_ota_activate() failed: %s -- coprocessor may need a manual power cycle",
                  esp_err_to_name(ret));
    }

    restart_host();
}
