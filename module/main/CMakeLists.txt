idf_component_register(SRCS "main.cpp" "app.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES cxx esp_driver_gpio esp_driver_i2c esp_event esp_http_client esp_http_server esp_netif esp_timer esp_wifi led_strip nvs_flash)

set(MODULE_NAME "" CACHE STRING "Logical module ID reported to the backend")
set(MODULE_TYPE "" CACHE STRING "Module type to build")
set(WIFI_SSID "example-network-name" CACHE STRING "Wi-Fi SSID")
set(WIFI_PASSWORD "example-pwd" CACHE STRING "Wi-Fi password")
set(SERVER_BASE_URL "http://192.168.4.1" CACHE STRING "HTTP server base URL")


if(MODULE_NAME STREQUAL "")
    message(FATAL_ERROR "MODULE_NAME must be set")
endif()
if(MODULE_TYPE STREQUAL "")
	message(FATAL_ERROR "MODULE_TYPE must be set")
endif()

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    MODULE_TYPE="${MODULE_TYPE}"
    MODULE_NAME="${MODULE_NAME}"
    WIFI_SSID="${WIFI_SSID}"
    WIFI_PASSWORD="${WIFI_PASSWORD}"
    SERVER_BASE_URL="${SERVER_BASE_URL}"
