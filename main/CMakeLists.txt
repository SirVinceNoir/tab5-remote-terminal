idf_component_register(SRCS "app_main.c" "wifi_manager.c" "wifi_creds.c" "wifi_cp_ota.c" "keyboard_indev.c" "ssh_spike.c" "ssh_session.c" "usb_serial_spike.c" "host_store.c" "ui_shell.c" "ui_wifi.c" "terminal.c" "telnet_client.c" "session_ui.c"
                        INCLUDE_DIRS "."
                        PRIV_REQUIRES driver bootloader_support nvs_flash esp_wifi esp_netif esp_event esp_partition lwip json)
