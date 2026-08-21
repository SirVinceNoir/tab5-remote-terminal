#pragma once

/*
 * Pushes the ESP32-C6 coprocessor firmware baked into the "slave_fw" data
 * partition over the already-up SDIO/RPC link, via esp_hosted's host-driven
 * OTA API. Call this after esp_wifi_init() succeeds (the RPC channel to the
 * coprocessor is live at that point) but before esp_wifi_start()/connect().
 *
 * No-ops quickly if the coprocessor already reports the same firmware
 * version baked into the partition. On a successful update, restarts the
 * host so both sides resync -- this function does not return in that case.
 */
void wifi_cp_ota_update_if_needed(void);
