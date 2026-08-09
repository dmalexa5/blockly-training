idf_component_register(SRCS "main.cpp" "app.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES cxx esp_driver_gpio esp_driver_i2c esp_event esp_http_client esp_http_server esp_netif esp_timer esp_wifi led_strip nvs_flash)

set(MODULE_NAME "" CACHE STRING "Logical module ID reported to the backend")
set(WIFI_SSID "example-network-name" CACHE STRING "Wi-Fi SSID")
set(WIFI_PASSWORD "example-pwd" CACHE STRING "Wi-Fi password")
set(SERVER_BASE_URL "http://192.168.4.1" CACHE STRING "HTTP server base URL")
set(BUTTON_GPIO 4 CACHE STRING "INPUT_PULLUP button GPIO")
set(LED_GPIO 48 CACHE STRING "LED strip GPIO")
set(LED_COUNT 1 CACHE STRING "LED count")
set(COMMS_PERIOD_MS 50 CACHE STRING "Backend event retry period in milliseconds")
if(MODULE_TYPE STREQUAL "rebounder")
    set(DEFAULT_SENSE_PERIOD_MS 1)
else()
    set(DEFAULT_SENSE_PERIOD_MS 10)
endif()
set(SENSE_PERIOD_MS ${DEFAULT_SENSE_PERIOD_MS} CACHE STRING "Sense task period in milliseconds")
set(CONTROL_PERIOD_MS 20 CACHE STRING "Control task period in milliseconds")
set(MPU_SDA_GPIO 8 CACHE STRING "MPU6050 SDA GPIO")
set(MPU_SCL_GPIO 9 CACHE STRING "MPU6050 SCL GPIO")
set(MPU_I2C_ADDR 0x68 CACHE STRING "MPU6050 I2C address")
set(ACCEL_THRESHOLD_G 1.2 CACHE STRING "Absolute acceleration threshold in g")
set(DEACTIVATE_IGNORE_MS 100 CACHE STRING "Rebounder inactive ignore window after deactivate")
set(ACCEL_LOG_PERIOD_MS 20 CACHE STRING "Rebounder acceleration log period in milliseconds")

if(MODULE_NAME STREQUAL "")
    message(FATAL_ERROR "MODULE_NAME must be set")
endif()

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    MODULE_TYPE="${MODULE_TYPE}"
    MODULE_NAME="${MODULE_NAME}"
    WIFI_SSID="${WIFI_SSID}"
    WIFI_PASSWORD="${WIFI_PASSWORD}"
    SERVER_BASE_URL="${SERVER_BASE_URL}"
    BUTTON_GPIO=${BUTTON_GPIO}
    LED_GPIO=${LED_GPIO}
    LED_COUNT=${LED_COUNT}
    COMMS_PERIOD_MS=${COMMS_PERIOD_MS}
    SENSE_PERIOD_MS=${SENSE_PERIOD_MS}
    CONTROL_PERIOD_MS=${CONTROL_PERIOD_MS}
    MPU_SDA_GPIO=${MPU_SDA_GPIO}
    MPU_SCL_GPIO=${MPU_SCL_GPIO}
    MPU_I2C_ADDR=${MPU_I2C_ADDR}
    ACCEL_THRESHOLD_G=${ACCEL_THRESHOLD_G}
    DEACTIVATE_IGNORE_MS=${DEACTIVATE_IGNORE_MS}
    ACCEL_LOG_PERIOD_MS=${ACCEL_LOG_PERIOD_MS})
