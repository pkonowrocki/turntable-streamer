idf_component_register(SRCS "main.c" "wifi_manager.c" "audio_streamer.c" "ota_manager.c"
                     INCLUDE_DIRS "."
                     REQUIRES nvs_flash esp_wifi esp_event log lwip esp_http_server esp_netif mdns
                              audio_pipeline esp_peripherals audio_hal audio_stream esp-adf-libs driver
                              esp_https_ota esp_http_client mbedtls app_update
)
