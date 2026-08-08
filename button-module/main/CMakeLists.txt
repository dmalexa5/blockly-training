idf_component_register(SRCS "main.cpp" "app.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES cxx esp_driver_gpio esp_event esp_http_client esp_netif esp_timer esp_wifi led_strip nvs_flash)

set(RBDR_WIFI_SSID "example-network-name" CACHE STRING "Wi-Fi SSID")
set(RBDR_WIFI_PASSWORD "example-pwd" CACHE STRING "Wi-Fi password")
set(RBDR_SERVER_BASE_URL "http://192.168.4.1" CACHE STRING "HTTP server base URL")
set(RBDR_BUTTON_GPIO 4 CACHE STRING "INPUT_PULLUP button GPIO")
set(RBDR_LED_GPIO 48 CACHE STRING "LED strip GPIO")
set(RBDR_LED_COUNT 1 CACHE STRING "LED count")
set(RBDR_COMMS_PERIOD_MS 50 CACHE STRING "Comms task period in milliseconds")
set(RBDR_SENSE_PERIOD_MS 10 CACHE STRING "Sense task period in milliseconds")
set(RBDR_CONTROL_PERIOD_MS 20 CACHE STRING "Control task period in milliseconds")

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    RBDR_WIFI_SSID="${RBDR_WIFI_SSID}"
    RBDR_WIFI_PASSWORD="${RBDR_WIFI_PASSWORD}"
    RBDR_SERVER_BASE_URL="${RBDR_SERVER_BASE_URL}"
    RBDR_BUTTON_GPIO=${RBDR_BUTTON_GPIO}
    RBDR_LED_GPIO=${RBDR_LED_GPIO}
    RBDR_LED_COUNT=${RBDR_LED_COUNT}
    RBDR_COMMS_PERIOD_MS=${RBDR_COMMS_PERIOD_MS}
    RBDR_SENSE_PERIOD_MS=${RBDR_SENSE_PERIOD_MS}
    RBDR_CONTROL_PERIOD_MS=${RBDR_CONTROL_PERIOD_MS})
