idf_component_register(SRCS "main.cpp" "app.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES cxx esp_driver_i2c esp_event esp_http_client esp_netif esp_timer esp_wifi led_strip nvs_flash)

set(RBDR_WIFI_SSID "example-network-name" CACHE STRING "Wi-Fi SSID")
set(RBDR_WIFI_PASSWORD "example-pwd" CACHE STRING "Wi-Fi password")
set(RBDR_SERVER_BASE_URL "http://192.168.4.1" CACHE STRING "HTTP server base URL")
set(RBDR_LED_GPIO 48 CACHE STRING "LED strip GPIO")
set(RBDR_LED_COUNT 1 CACHE STRING "LED count")
set(RBDR_COMMS_PERIOD_MS 50 CACHE STRING "Comms task period in milliseconds")
set(RBDR_SENSE_PERIOD_MS 10 CACHE STRING "Sense task period in milliseconds")
set(RBDR_CONTROL_PERIOD_MS 20 CACHE STRING "Control task period in milliseconds")
set(RBDR_MPU_SDA_GPIO 8 CACHE STRING "MPU6050 I2C SDA GPIO")
set(RBDR_MPU_SCL_GPIO 9 CACHE STRING "MPU6050 I2C SCL GPIO")
set(RBDR_MPU_I2C_ADDR 0x68 CACHE STRING "MPU6050 I2C address")
set(RBDR_ACCEL_THRESHOLD_G 2.0 CACHE STRING "Filtered acceleration trigger threshold in g")
set(RBDR_ACCEL_EMA_ALPHA 0.6 CACHE STRING "Acceleration cascaded EMA alpha")
set(RBDR_DEACTIVATE_IGNORE_MS 100 CACHE STRING "Inactive trigger ignore period after deactivate in milliseconds")
set(RBDR_ACCEL_LOG_PERIOD_MS 1000 CACHE STRING "Filtered acceleration log period in milliseconds")

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    RBDR_WIFI_SSID="${RBDR_WIFI_SSID}"
    RBDR_WIFI_PASSWORD="${RBDR_WIFI_PASSWORD}"
    RBDR_SERVER_BASE_URL="${RBDR_SERVER_BASE_URL}"
    RBDR_LED_GPIO=${RBDR_LED_GPIO}
    RBDR_LED_COUNT=${RBDR_LED_COUNT}
    RBDR_COMMS_PERIOD_MS=${RBDR_COMMS_PERIOD_MS}
    RBDR_SENSE_PERIOD_MS=${RBDR_SENSE_PERIOD_MS}
    RBDR_CONTROL_PERIOD_MS=${RBDR_CONTROL_PERIOD_MS}
    RBDR_MPU_SDA_GPIO=${RBDR_MPU_SDA_GPIO}
    RBDR_MPU_SCL_GPIO=${RBDR_MPU_SCL_GPIO}
    RBDR_MPU_I2C_ADDR=${RBDR_MPU_I2C_ADDR}
    RBDR_ACCEL_THRESHOLD_G=${RBDR_ACCEL_THRESHOLD_G}
    RBDR_ACCEL_EMA_ALPHA=${RBDR_ACCEL_EMA_ALPHA}
    RBDR_DEACTIVATE_IGNORE_MS=${RBDR_DEACTIVATE_IGNORE_MS}
    RBDR_ACCEL_LOG_PERIOD_MS=${RBDR_ACCEL_LOG_PERIOD_MS})
