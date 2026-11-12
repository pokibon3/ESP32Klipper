# WiFi connection with credentials in NVS, a setup access point (captive
# portal) when no network is reachable, and the setup page /wifi.
idf_component_register(SRCS src/wifi_setup.cpp src/setup_page.cpp
                       INCLUDE_DIRS include
                       REQUIRES esp_wifi esp_netif esp_event esp_http_server
                                nvs_flash lwip freertos esp_timer)
