#include "app.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cinttypes>
#include <cstring>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "led_strip_rmt.h"
#include "nvs_flash.h"

#ifndef RBDR_WIFI_SSID
#define RBDR_WIFI_SSID "example-network-name"
#endif

#ifndef RBDR_WIFI_PASSWORD
#define RBDR_WIFI_PASSWORD "example-pwd"
#endif

#ifndef RBDR_SERVER_BASE_URL
#define RBDR_SERVER_BASE_URL "http://192.168.4.1"
#endif

#ifndef RBDR_LED_GPIO
#define RBDR_LED_GPIO 48
#endif

#ifndef RBDR_LED_COUNT
#define RBDR_LED_COUNT 1
#endif

#ifndef RBDR_COMMS_PERIOD_MS
#define RBDR_COMMS_PERIOD_MS 50
#endif

#ifndef RBDR_SENSE_PERIOD_MS
#define RBDR_SENSE_PERIOD_MS 10
#endif

#ifndef RBDR_CONTROL_PERIOD_MS
#define RBDR_CONTROL_PERIOD_MS 20
#endif

#ifndef RBDR_MPU_SDA_GPIO
#define RBDR_MPU_SDA_GPIO 8
#endif

#ifndef RBDR_MPU_SCL_GPIO
#define RBDR_MPU_SCL_GPIO 9
#endif

#ifndef RBDR_MPU_I2C_ADDR
#define RBDR_MPU_I2C_ADDR 0x68
#endif

#ifndef RBDR_ACCEL_THRESHOLD_G
#define RBDR_ACCEL_THRESHOLD_G 2.0f
#endif

#ifndef RBDR_ACCEL_EMA_ALPHA
#define RBDR_ACCEL_EMA_ALPHA 0.6f
#endif

#ifndef RBDR_DEACTIVATE_IGNORE_MS
#define RBDR_DEACTIVATE_IGNORE_MS 100
#endif

#ifndef RBDR_ACCEL_LOG_PERIOD_MS
#define RBDR_ACCEL_LOG_PERIOD_MS 1000
#endif

static_assert(RBDR_ACCEL_EMA_ALPHA > 0.0f && RBDR_ACCEL_EMA_ALPHA <= 1.0f,
              "RBDR_ACCEL_EMA_ALPHA must be > 0 and <= 1");

namespace {

constexpr char kModuleName[] = "rebounder";
constexpr int kCommandQueueLen = 8;
constexpr int kHttpTimeoutMs = 1000;
constexpr int kFlashDurationMs = 1000;
constexpr int kMpuRetryMs = 1000;
constexpr int kMpuI2cTimeoutMs = 100;
constexpr float kAccelLsbPerG = 8192.0f;
constexpr EventBits_t kWifiConnectedBit = BIT0;

constexpr uint8_t kMpuRegAccelConfig = 0x1C;
constexpr uint8_t kMpuRegAccelXoutH = 0x3B;
constexpr uint8_t kMpuRegConfig = 0x1A;
constexpr uint8_t kMpuRegPwrMgmt1 = 0x6B;
constexpr uint8_t kMpuRegWhoAmI = 0x75;

const char *TAG = "rebounder";

enum class command_t {
    activate,
    deactivate,
};

struct mod_state_t {
    bool rising_edge;
    float magnitude_g;
    uint32_t tstamp_ms;
};

struct axis_sample_t {
    float x;
    float y;
    float z;
};

struct ema_filter_t {
    axis_sample_t stage1;
    axis_sample_t stage2;
    axis_sample_t stage3;
    bool seeded;
};

struct mpu_t {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    bool initialized;
};

struct outbound_msg_t {
    char event[24];
    char reason[32];
    bool active;
    bool triggered;
    uint32_t uptime_ms;
};

struct http_response_t {
    char body[256];
    int len;
};

QueueHandle_t g_command_queue = nullptr;
SemaphoreHandle_t g_state_mutex = nullptr;
SemaphoreHandle_t g_send_mutex = nullptr;
EventGroupHandle_t g_wifi_event_group = nullptr;
TaskHandle_t g_comms_task_handle = nullptr;
led_strip_handle_t g_led_strip = nullptr;

mod_state_t g_latest_state = {};
outbound_msg_t g_send_slot = {};
bool g_send_slot_full = false;
std::atomic_bool g_wifi_connected{false};
std::atomic_bool g_reset_sensor_filter{true};

uint32_t uptime_ms()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA || evt->data == nullptr || evt->user_data == nullptr) {
        return ESP_OK;
    }

    auto *response = static_cast<http_response_t *>(evt->user_data);
    const int remaining = static_cast<int>(sizeof(response->body) - 1) - response->len;
    if (remaining <= 0) {
        return ESP_OK;
    }

    const int copy_len = evt->data_len < remaining ? evt->data_len : remaining;
    memcpy(response->body + response->len, evt->data, copy_len);
    response->len += copy_len;
    response->body[response->len] = '\0';
    return ESP_OK;
}

bool parse_command(const char *json, command_t *command)
{
    if (json == nullptr) {
        return false;
    }

    if (strstr(json, "\"cmd\"") != nullptr && strstr(json, "\"activate\"") != nullptr) {
        *command = command_t::activate;
        return true;
    }

    if (strstr(json, "\"cmd\"") != nullptr && strstr(json, "\"deactivate\"") != nullptr) {
        *command = command_t::deactivate;
        return true;
    }

    return false;
}

bool poll_command(command_t *command)
{
    char url[160];
    snprintf(url, sizeof(url), "%s/poll?module=%s", RBDR_SERVER_BASE_URL, kModuleName);

    http_response_t response = {};
    esp_http_client_config_t config = {};
    config.url = url;
    config.timeout_ms = kHttpTimeoutMs;
    config.event_handler = http_event_handler;
    config.user_data = &response;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        ESP_LOGW(TAG, "failed to init poll client");
        return false;
    }

    esp_http_client_set_method(client, HTTP_METHOD_GET);
    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "poll failed: %s", esp_err_to_name(err));
        return false;
    }

    if (status == 204) {
        return false;
    }

    if (status != 200) {
        ESP_LOGW(TAG, "unexpected poll status: %d", status);
        return false;
    }

    if (!parse_command(response.body, command)) {
        ESP_LOGW(TAG, "invalid command json: %s", response.body);
        return false;
    }

    return true;
}

bool build_event_json(const outbound_msg_t &msg, char *json, size_t json_len)
{
    if (msg.reason[0] != '\0') {
        const int written = snprintf(json,
                                     json_len,
                                     "{\"event\":\"%s\",\"module\":\"%s\",\"active\":%s,\"triggered\":%s,\"uptime_ms\":%" PRIu32 ",\"reason\":\"%s\"}",
                                     msg.event,
                                     kModuleName,
                                     msg.active ? "true" : "false",
                                     msg.triggered ? "true" : "false",
                                     msg.uptime_ms,
                                     msg.reason);
        return written > 0 && static_cast<size_t>(written) < json_len;
    }

    const int written = snprintf(json,
                                 json_len,
                                 "{\"event\":\"%s\",\"module\":\"%s\",\"active\":%s,\"triggered\":%s,\"uptime_ms\":%" PRIu32 "}",
                                 msg.event,
                                 kModuleName,
                                 msg.active ? "true" : "false",
                                 msg.triggered ? "true" : "false",
                                 msg.uptime_ms);
    return written > 0 && static_cast<size_t>(written) < json_len;
}

void post_event(const outbound_msg_t &msg)
{
    char url[160];
    char json[192];
    snprintf(url, sizeof(url), "%s/events", RBDR_SERVER_BASE_URL);

    if (!build_event_json(msg, json, sizeof(json))) {
        ESP_LOGW(TAG, "failed to build event json");
        return;
    }

    esp_http_client_config_t config = {};
    config.url = url;
    config.timeout_ms = kHttpTimeoutMs;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        ESP_LOGW(TAG, "failed to init post client");
        return;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json, strlen(json));

    const esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "post failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "posted event status=%d body=%s", esp_http_client_get_status_code(client), json);
    }

    esp_http_client_cleanup(client);
}

void write_event(const char *event, const char *reason, bool active, bool triggered)
{
    outbound_msg_t msg = {};
    strlcpy(msg.event, event, sizeof(msg.event));
    if (reason != nullptr) {
        strlcpy(msg.reason, reason, sizeof(msg.reason));
    }
    msg.active = active;
    msg.triggered = triggered;
    msg.uptime_ms = uptime_ms();

    if (msg.reason[0] != '\0') {
        ESP_LOGI(TAG,
                 "event detected event=%s reason=%s active=%s triggered=%s uptime_ms=%" PRIu32 " wifi_connected=%s",
                 msg.event,
                 msg.reason,
                 msg.active ? "true" : "false",
                 msg.triggered ? "true" : "false",
                 msg.uptime_ms,
                 g_wifi_connected.load() ? "true" : "false");
    } else {
        ESP_LOGI(TAG,
                 "event detected event=%s active=%s triggered=%s uptime_ms=%" PRIu32 " wifi_connected=%s",
                 msg.event,
                 msg.active ? "true" : "false",
                 msg.triggered ? "true" : "false",
                 msg.uptime_ms,
                 g_wifi_connected.load() ? "true" : "false");
    }

    xSemaphoreTake(g_send_mutex, portMAX_DELAY);
    g_send_slot = msg;
    g_send_slot_full = true;
    xSemaphoreGive(g_send_mutex);

    if (g_comms_task_handle != nullptr) {
        xTaskNotifyGive(g_comms_task_handle);
    }
}

bool take_event(outbound_msg_t *msg)
{
    bool available = false;
    xSemaphoreTake(g_send_mutex, portMAX_DELAY);
    if (g_send_slot_full) {
        *msg = g_send_slot;
        g_send_slot_full = false;
        available = true;
    }
    xSemaphoreGive(g_send_mutex);
    return available;
}

void set_led_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    if (g_led_strip == nullptr) {
        return;
    }

    for (int i = 0; i < RBDR_LED_COUNT; ++i) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(led_strip_set_pixel(g_led_strip, i, red, green, blue));
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(led_strip_refresh(g_led_strip));
}

void clear_led()
{
    if (g_led_strip != nullptr) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(led_strip_clear(g_led_strip));
    }
}

void wifi_event_handler(void *, esp_event_base_t event_base, int32_t event_id, void *)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        g_wifi_connected.store(false);
        xEventGroupClearBits(g_wifi_event_group, kWifiConnectedBit);
        esp_wifi_connect();
        ESP_LOGW(TAG, "wifi disconnected, retrying");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        g_wifi_connected.store(true);
        xEventGroupSetBits(g_wifi_event_group, kWifiConnectedBit);
        ESP_LOGI(TAG, "wifi connected");
    }
}

void init_wifi()
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    g_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, nullptr, nullptr));

    wifi_config_t wifi_config = {};
    strlcpy(reinterpret_cast<char *>(wifi_config.sta.ssid), RBDR_WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strlcpy(reinterpret_cast<char *>(wifi_config.sta.password), RBDR_WIFI_PASSWORD, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = strlen(RBDR_WIFI_PASSWORD) == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

void init_led()
{
    led_strip_config_t strip_config = {};
    strip_config.strip_gpio_num = RBDR_LED_GPIO;
    strip_config.max_leds = RBDR_LED_COUNT;
    strip_config.led_model = LED_MODEL_WS2812;
    strip_config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;

    led_strip_rmt_config_t rmt_config = {};
    rmt_config.resolution_hz = 10 * 1000 * 1000;
    rmt_config.flags.with_dma = false;

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &g_led_strip));
    clear_led();
}

esp_err_t mpu_write_reg(mpu_t &mpu, uint8_t reg, uint8_t value)
{
    const uint8_t tx[] = {reg, value};
    return i2c_master_transmit(mpu.dev, tx, sizeof(tx), kMpuI2cTimeoutMs);
}

esp_err_t mpu_read_reg(mpu_t &mpu, uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(mpu.dev, &reg, 1, value, 1, kMpuI2cTimeoutMs);
}

void mpu_deinit(mpu_t &mpu)
{
    if (mpu.dev != nullptr) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_master_bus_rm_device(mpu.dev));
        mpu.dev = nullptr;
    }
    if (mpu.bus != nullptr) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_del_master_bus(mpu.bus));
        mpu.bus = nullptr;
    }
    mpu.initialized = false;
}

esp_err_t mpu_init(mpu_t &mpu)
{
    mpu_deinit(mpu);

    i2c_master_bus_config_t bus_config = {};
    bus_config.i2c_port = I2C_NUM_0;
    bus_config.sda_io_num = static_cast<gpio_num_t>(RBDR_MPU_SDA_GPIO);
    bus_config.scl_io_num = static_cast<gpio_num_t>(RBDR_MPU_SCL_GPIO);
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.flags.enable_internal_pullup = true;

    esp_err_t err = i2c_new_master_bus(&bus_config, &mpu.bus);
    if (err != ESP_OK) {
        mpu_deinit(mpu);
        return err;
    }

    i2c_device_config_t dev_config = {};
    dev_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_config.device_address = RBDR_MPU_I2C_ADDR;
    dev_config.scl_speed_hz = 400000;

    err = i2c_master_bus_add_device(mpu.bus, &dev_config, &mpu.dev);
    if (err != ESP_OK) {
        mpu_deinit(mpu);
        return err;
    }

    uint8_t who_am_i = 0;
    err = mpu_read_reg(mpu, kMpuRegWhoAmI, &who_am_i);
    if (err != ESP_OK) {
        mpu_deinit(mpu);
        return err;
    }
    if (who_am_i != 0x68) {
        ESP_LOGW(TAG, "unexpected MPU WHO_AM_I: 0x%02x", who_am_i);
        mpu_deinit(mpu);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if ((err = mpu_write_reg(mpu, kMpuRegPwrMgmt1, 0x00)) != ESP_OK ||
        (err = mpu_write_reg(mpu, kMpuRegConfig, 0x03)) != ESP_OK ||
        (err = mpu_write_reg(mpu, kMpuRegAccelConfig, 0x08)) != ESP_OK) {
        mpu_deinit(mpu);
        return err;
    }

    mpu.initialized = true;
    ESP_LOGI(TAG, "MPU6050 initialized");
    return ESP_OK;
}

esp_err_t mpu_read_accel_g(mpu_t &mpu, axis_sample_t *sample)
{
    const uint8_t reg = kMpuRegAccelXoutH;
    uint8_t rx[6] = {};
    const esp_err_t err = i2c_master_transmit_receive(mpu.dev, &reg, 1, rx, sizeof(rx), kMpuI2cTimeoutMs);
    if (err != ESP_OK) {
        return err;
    }

    const int16_t raw_x = static_cast<int16_t>((rx[0] << 8) | rx[1]);
    const int16_t raw_y = static_cast<int16_t>((rx[2] << 8) | rx[3]);
    const int16_t raw_z = static_cast<int16_t>((rx[4] << 8) | rx[5]);
    sample->x = static_cast<float>(raw_x) / kAccelLsbPerG;
    sample->y = static_cast<float>(raw_y) / kAccelLsbPerG;
    sample->z = static_cast<float>(raw_z) / kAccelLsbPerG;
    return ESP_OK;
}

axis_sample_t ema_axis(const axis_sample_t &previous, const axis_sample_t &current)
{
    constexpr float alpha = RBDR_ACCEL_EMA_ALPHA;
    constexpr float beta = 1.0f - alpha;
    return {
        alpha * current.x + beta * previous.x,
        alpha * current.y + beta * previous.y,
        alpha * current.z + beta * previous.z,
    };
}

axis_sample_t update_filter(ema_filter_t &filter, const axis_sample_t &sample)
{
    if (!filter.seeded) {
        filter.stage1 = sample;
        filter.stage2 = sample;
        filter.stage3 = sample;
        filter.seeded = true;
        return filter.stage3;
    }

    filter.stage1 = ema_axis(filter.stage1, sample);
    filter.stage2 = ema_axis(filter.stage2, filter.stage1);
    filter.stage3 = ema_axis(filter.stage3, filter.stage2);
    return filter.stage3;
}

void reset_filter_state(ema_filter_t &filter, bool &over_threshold)
{
    filter = {};
    over_threshold = false;
}

void comms_task(void *)
{
    g_comms_task_handle = xTaskGetCurrentTaskHandle();

    while (true) {
        xEventGroupWaitBits(g_wifi_event_group, kWifiConnectedBit, pdFALSE, pdTRUE, pdMS_TO_TICKS(1000));

        outbound_msg_t msg = {};
        if (take_event(&msg) && g_wifi_connected.load()) {
            post_event(msg);
        }

        if (g_wifi_connected.load()) {
            command_t command;
            if (poll_command(&command)) {
                xQueueSend(g_command_queue, &command, 0);
            }
        }

        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(RBDR_COMMS_PERIOD_MS));
    }
}

void sense_task(void *)
{
    mpu_t mpu = {};
    ema_filter_t filter = {};
    bool over_threshold = false;
    uint32_t next_retry_ms = 0;
    uint32_t next_log_ms = 0;
    bool read_failure_logged = false;

    while (true) {
        if (g_reset_sensor_filter.exchange(false)) {
            reset_filter_state(filter, over_threshold);
        }

        const uint32_t now_ms = uptime_ms();
        if (!mpu.initialized) {
            if (static_cast<int32_t>(now_ms - next_retry_ms) >= 0) {
                const esp_err_t err = mpu_init(mpu);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "MPU6050 init failed: %s", esp_err_to_name(err));
                    next_retry_ms = now_ms + kMpuRetryMs;
                } else {
                    reset_filter_state(filter, over_threshold);
                    read_failure_logged = false;
                    next_log_ms = now_ms + RBDR_ACCEL_LOG_PERIOD_MS;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(RBDR_SENSE_PERIOD_MS));
            continue;
        }

        axis_sample_t raw_sample = {};
        const esp_err_t err = mpu_read_accel_g(mpu, &raw_sample);
        if (err != ESP_OK) {
            if (!read_failure_logged) {
                ESP_LOGW(TAG, "MPU6050 read failed: %s", esp_err_to_name(err));
                read_failure_logged = true;
            }
            mpu_deinit(mpu);
            next_retry_ms = now_ms + kMpuRetryMs;
            reset_filter_state(filter, over_threshold);
            vTaskDelay(pdMS_TO_TICKS(RBDR_SENSE_PERIOD_MS));
            continue;
        }

        if (read_failure_logged) {
            ESP_LOGI(TAG, "MPU6050 read recovered");
            read_failure_logged = false;
        }

        const bool was_seeded = filter.seeded;
        const axis_sample_t filtered = update_filter(filter, raw_sample);
        const float magnitude_g = sqrtf(filtered.x * filtered.x + filtered.y * filtered.y + filtered.z * filtered.z);
        const bool is_over_threshold = magnitude_g > RBDR_ACCEL_THRESHOLD_G;
        const bool rising_edge = was_seeded && !over_threshold && is_over_threshold;
        over_threshold = is_over_threshold;

        mod_state_t state = {};
        state.rising_edge = rising_edge;
        state.magnitude_g = magnitude_g;
        state.tstamp_ms = now_ms;

        xSemaphoreTake(g_state_mutex, portMAX_DELAY);
        g_latest_state = state;
        xSemaphoreGive(g_state_mutex);

        if (RBDR_ACCEL_LOG_PERIOD_MS > 0 && static_cast<int32_t>(now_ms - next_log_ms) >= 0) {
            ESP_LOGI(TAG, "filtered acceleration magnitude=%.3fg", magnitude_g);
            next_log_ms = now_ms + RBDR_ACCEL_LOG_PERIOD_MS;
        }

        vTaskDelay(pdMS_TO_TICKS(RBDR_SENSE_PERIOD_MS));
    }
}

void control_task(void *)
{
    bool active = false;
    bool triggered = false;
    uint32_t inactive_flash_until_ms = 0;
    uint32_t inactive_ignore_until_ms = 0;
    bool led_flash_on = false;

    while (true) {
        command_t command;
        while (xQueueReceive(g_command_queue, &command, 0) == pdTRUE) {
            if (command == command_t::activate) {
                if (triggered) {
                    write_event("error", "already_triggered", active, triggered);
                } else {
                    active = true;
                    inactive_flash_until_ms = 0;
                    g_reset_sensor_filter.store(true);
                }
            } else if (command == command_t::deactivate) {
                active = false;
                triggered = false;
                inactive_flash_until_ms = 0;
                inactive_ignore_until_ms = uptime_ms() + RBDR_DEACTIVATE_IGNORE_MS;
            }
        }

        mod_state_t state = {};
        xSemaphoreTake(g_state_mutex, portMAX_DELAY);
        state = g_latest_state;
        xSemaphoreGive(g_state_mutex);

        const uint32_t now_ms = uptime_ms();
        if (state.rising_edge) {
            if (active) {
                if (!triggered) {
                    triggered = true;
                    write_event("triggered", nullptr, active, triggered);
                }
            } else if (static_cast<int32_t>(now_ms - inactive_ignore_until_ms) >= 0) {
                triggered = true;
                inactive_flash_until_ms = now_ms + kFlashDurationMs;
                ESP_LOGI(TAG,
                         "event detected event=rebound active=false triggered=true magnitude_g=%.3f uptime_ms=%" PRIu32 " wifi_connected=%s",
                         state.magnitude_g,
                         state.tstamp_ms,
                         g_wifi_connected.load() ? "true" : "false");
            }
        }

        if (!active && triggered && inactive_flash_until_ms != 0 && static_cast<int32_t>(now_ms - inactive_flash_until_ms) >= 0) {
            triggered = false;
            inactive_flash_until_ms = 0;
        }

        if (active && !triggered) {
            set_led_rgb(0, 0, 32);
        } else if (!active && triggered && inactive_flash_until_ms != 0) {
            led_flash_on = !led_flash_on;
            if (led_flash_on) {
                set_led_rgb(32, 0, 0);
            } else {
                clear_led();
            }
        } else {
            clear_led();
        }

        vTaskDelay(pdMS_TO_TICKS(RBDR_CONTROL_PERIOD_MS));
    }
}

} // namespace

void rebounder_app_start()
{
    g_command_queue = xQueueCreate(kCommandQueueLen, sizeof(command_t));
    g_state_mutex = xSemaphoreCreateMutex();
    g_send_mutex = xSemaphoreCreateMutex();

    ESP_ERROR_CHECK(g_command_queue == nullptr ? ESP_ERR_NO_MEM : ESP_OK);
    ESP_ERROR_CHECK(g_state_mutex == nullptr ? ESP_ERR_NO_MEM : ESP_OK);
    ESP_ERROR_CHECK(g_send_mutex == nullptr ? ESP_ERR_NO_MEM : ESP_OK);

    init_led();
    init_wifi();

    xTaskCreate(comms_task, "comms_task", 6144, nullptr, 4, &g_comms_task_handle);
    xTaskCreate(sense_task, "sense_task", 3072, nullptr, 5, nullptr);
    xTaskCreate(control_task, "control_task", 4096, nullptr, 5, nullptr);
}
