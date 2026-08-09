#include "app.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "led_strip_rmt.h"
#include "nvs_flash.h"

#ifndef MODULE_TYPE
#define MODULE_TYPE "button"
#endif

#ifndef MODULE_NAME
#define MODULE_NAME "module"
#endif

#ifndef WIFI_SSID
#define WIFI_SSID "example-network-name"
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "example-pwd"
#endif

#ifndef SERVER_BASE_URL
#define SERVER_BASE_URL "http://192.168.4.1"
#endif

#ifndef BUTTON_GPIO
#define BUTTON_GPIO 4
#endif

#ifndef LED_GPIO
#define LED_GPIO 48
#endif

#ifndef LED_COUNT
#define LED_COUNT 1
#endif

#ifndef COMMS_PERIOD_MS
#define COMMS_PERIOD_MS 50
#endif

#ifndef SENSE_PERIOD_MS
#define SENSE_PERIOD_MS 10
#endif

#ifndef CONTROL_PERIOD_MS
#define CONTROL_PERIOD_MS 20
#endif

#ifndef MPU_SDA_GPIO
#define MPU_SDA_GPIO 8
#endif

#ifndef MPU_SCL_GPIO
#define MPU_SCL_GPIO 9
#endif

#ifndef MPU_I2C_ADDR
#define MPU_I2C_ADDR 0x68
#endif

#ifndef ACCEL_THRESHOLD_G
#define ACCEL_THRESHOLD_G 1.2f
#endif

#ifndef DEACTIVATE_IGNORE_MS
#define DEACTIVATE_IGNORE_MS 100
#endif

#ifndef ACCEL_LOG_PERIOD_MS
#define ACCEL_LOG_PERIOD_MS 20
#endif

namespace {

constexpr char kModuleName[] = MODULE_NAME;
constexpr char kModuleType[] = MODULE_TYPE;
constexpr int kHttpTimeoutMs = 1000;
constexpr int kDebounceStableMs = 50;
constexpr int kFlashDurationMs = 1000;
constexpr int kMpuRetryMs = 1000;
constexpr int kMpuI2cTimeoutMs = 100;
constexpr float kAccelLsbPerG = 8192.0f;
constexpr EventBits_t kWifiConnectedBit = BIT0;

constexpr uint8_t kMpuRegAccelConfig = 0x1C;
constexpr uint8_t kMpuRegAccelXoutH = 0x3B;
constexpr uint8_t kMpuRegConfig = 0x1A;
constexpr uint8_t kMpuRegPwrMgmt1 = 0x6B;
constexpr uint8_t kMpuRegSampleRateDiv = 0x19;
constexpr uint8_t kMpuRegWhoAmI = 0x75;

const char *TAG = "module";

enum class command_t {
    none,
    activate,
    deactivate,
};

enum class sensor_kind_t {
    button,
    rebounder,
};

struct button_sens_t {
    bool pressed;
    bool fell;
    bool rose;
    uint32_t tstamp_ms;
};

struct rebounder_sens_t {
    float magnitude_g;
    bool rising_edge;
    uint32_t tstamp_ms;
};

struct state_desr_t {
    command_t command;
    bool command_pending;
    uint32_t command_seq;
    bool event_ack_pending;
    uint32_t event_ack_seq;
};

struct state_sens_t {
    sensor_kind_t kind;
    union {
        button_sens_t button;
        rebounder_sens_t rebounder;
    };
};

struct state_ctrl_t {
    bool active;
    bool triggered;
    bool event_pending;
    uint32_t event_seq;
    uint32_t command_seq_seen;
    char event[24];
};

struct axis_sample_t {
    float x;
    float y;
    float z;
};

struct mpu_t {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    bool initialized;
};

SemaphoreHandle_t g_desr_mutex = nullptr;
SemaphoreHandle_t g_sens_mutex = nullptr;
SemaphoreHandle_t g_ctrl_mutex = nullptr;
EventGroupHandle_t g_wifi_event_group = nullptr;
led_strip_handle_t g_led_strip = nullptr;
httpd_handle_t g_httpd = nullptr;

state_desr_t g_desr = {};
state_sens_t g_sens = {};
state_ctrl_t g_ctrl = {};
std::atomic_bool g_wifi_connected{false};
std::atomic_bool g_reset_sensor_filter{true};

uint32_t uptime_ms()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

bool module_is_button()
{
    return strcmp(kModuleType, "button") == 0;
}

bool module_is_rebounder()
{
    return strcmp(kModuleType, "rebounder") == 0;
}

void state_desr_read(state_desr_t *out)
{
    xSemaphoreTake(g_desr_mutex, portMAX_DELAY);
    *out = g_desr;
    xSemaphoreGive(g_desr_mutex);
}

void state_desr_write(const state_desr_t &in)
{
    xSemaphoreTake(g_desr_mutex, portMAX_DELAY);
    g_desr = in;
    xSemaphoreGive(g_desr_mutex);
}

void state_sens_read(state_sens_t *out)
{
    xSemaphoreTake(g_sens_mutex, portMAX_DELAY);
    *out = g_sens;
    if (g_sens.kind == sensor_kind_t::button) {
        g_sens.button.fell = false;
        g_sens.button.rose = false;
    } else {
        g_sens.rebounder.rising_edge = false;
    }
    xSemaphoreGive(g_sens_mutex);
}

void state_sens_write(const state_sens_t &in)
{
    xSemaphoreTake(g_sens_mutex, portMAX_DELAY);
    state_sens_t next = in;
    if (next.kind == sensor_kind_t::button && g_sens.kind == sensor_kind_t::button) {
        next.button.fell = next.button.fell || g_sens.button.fell;
        next.button.rose = next.button.rose || g_sens.button.rose;
    }
    if (next.kind == sensor_kind_t::rebounder && g_sens.kind == sensor_kind_t::rebounder) {
        next.rebounder.rising_edge = next.rebounder.rising_edge || g_sens.rebounder.rising_edge;
    }
    g_sens = next;
    xSemaphoreGive(g_sens_mutex);
}

void state_ctrl_read(state_ctrl_t *out)
{
    xSemaphoreTake(g_ctrl_mutex, portMAX_DELAY);
    *out = g_ctrl;
    xSemaphoreGive(g_ctrl_mutex);
}

void state_ctrl_write(const state_ctrl_t &in)
{
    xSemaphoreTake(g_ctrl_mutex, portMAX_DELAY);
    g_ctrl = in;
    xSemaphoreGive(g_ctrl_mutex);
}

int hal_read_button_gpio_level()
{
    return gpio_get_level(static_cast<gpio_num_t>(BUTTON_GPIO));
}

void hal_led_set_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    if (g_led_strip == nullptr) {
        return;
    }
    for (int i = 0; i < LED_COUNT; ++i) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(led_strip_set_pixel(g_led_strip, i, red, green, blue));
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(led_strip_refresh(g_led_strip));
}

void hal_led_clear()
{
    if (g_led_strip != nullptr) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(led_strip_clear(g_led_strip));
    }
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
    bus_config.sda_io_num = static_cast<gpio_num_t>(MPU_SDA_GPIO);
    bus_config.scl_io_num = static_cast<gpio_num_t>(MPU_SCL_GPIO);
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.flags.enable_internal_pullup = true;

    esp_err_t err = i2c_new_master_bus(&bus_config, &mpu.bus);
    if (err != ESP_OK) {
        mpu_deinit(mpu);
        return err;
    }

    i2c_device_config_t dev_config = {};
    dev_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_config.device_address = MPU_I2C_ADDR;
    dev_config.scl_speed_hz = 400000;

    err = i2c_master_bus_add_device(mpu.bus, &dev_config, &mpu.dev);
    if (err != ESP_OK) {
        mpu_deinit(mpu);
        return err;
    }

    uint8_t who_am_i = 0;
    err = mpu_read_reg(mpu, kMpuRegWhoAmI, &who_am_i);
    if (err != ESP_OK || who_am_i != 0x68) {
        ESP_LOGW(TAG, "unexpected MPU WHO_AM_I: 0x%02x", who_am_i);
        mpu_deinit(mpu);
        return err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
    }

    if ((err = mpu_write_reg(mpu, kMpuRegPwrMgmt1, 0x00)) != ESP_OK ||
        (err = mpu_write_reg(mpu, kMpuRegSampleRateDiv, 0x07)) != ESP_OK ||
        (err = mpu_write_reg(mpu, kMpuRegConfig, 0x00)) != ESP_OK ||
        (err = mpu_write_reg(mpu, kMpuRegAccelConfig, 0x08)) != ESP_OK) {
        mpu_deinit(mpu);
        return err;
    }

    mpu.initialized = true;
    ESP_LOGI(TAG, "MPU6050 initialized");
    return ESP_OK;
}

esp_err_t hal_read_mpu_accel(axis_sample_t *sample, mpu_t &mpu)
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

bool post_pending_event(const state_ctrl_t &ctrl)
{
    char url[160];
    char json[96];
    snprintf(url, sizeof(url), "%s/events", SERVER_BASE_URL);
    const int written = snprintf(json, sizeof(json), "{\"module\":\"%s\",\"event\":\"%s\"}", kModuleName, ctrl.event);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(json)) {
        return false;
    }

    esp_http_client_config_t config = {};
    config.url = url;
    config.timeout_ms = kHttpTimeoutMs;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return false;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json, strlen(json));

    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "event post failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "event post status=%d body=%s", status, json);
    return status >= 200 && status < 300;
}

esp_err_t health_handler(httpd_req_t *req)
{
    char json[96];
    snprintf(json, sizeof(json), "{\"module\":\"%s\",\"type\":\"%s\"}", kModuleName, kModuleType);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

esp_err_t command_handler(httpd_req_t *req)
{
    char body[96] = {};
    int remaining = req->content_len;
    if (remaining <= 0 || remaining >= static_cast<int>(sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid command");
        return ESP_OK;
    }
    int offset = 0;
    while (remaining > 0) {
        const int read = httpd_req_recv(req, body + offset, remaining);
        if (read <= 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "read failed");
            return ESP_OK;
        }
        offset += read;
        remaining -= read;
    }

    command_t command = command_t::none;
    if (!parse_command(body, &command)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid command");
        return ESP_OK;
    }

    state_ctrl_t ctrl = {};
    state_ctrl_read(&ctrl);

    state_desr_t desired = {};
    state_desr_read(&desired);
    if (desired.command_pending && desired.command_seq == ctrl.command_seq_seen) {
        desired.command_pending = false;
        state_desr_write(desired);
    }
    if (desired.command_pending) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_send(req, "busy", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    desired.command = command;
    desired.command_pending = true;
    desired.command_seq += 1;
    state_desr_write(desired);

    httpd_resp_set_status(req, "202 Accepted");
    return httpd_resp_send(req, nullptr, 0);
}

void start_http_server()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    ESP_ERROR_CHECK(httpd_start(&g_httpd, &config));

    httpd_uri_t health = {};
    health.uri = "/health";
    health.method = HTTP_GET;
    health.handler = health_handler;
    ESP_ERROR_CHECK(httpd_register_uri_handler(g_httpd, &health));

    httpd_uri_t command = {};
    command.uri = "/command";
    command.method = HTTP_POST;
    command.handler = command_handler;
    ESP_ERROR_CHECK(httpd_register_uri_handler(g_httpd, &command));
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
    strlcpy(reinterpret_cast<char *>(wifi_config.sta.ssid), WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strlcpy(reinterpret_cast<char *>(wifi_config.sta.password), WIFI_PASSWORD, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = strlen(WIFI_PASSWORD) == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

void init_button()
{
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = 1ULL << BUTTON_GPIO;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    ESP_LOGI(TAG,
             "button gpio configured gpio=%d mode=input pullup=enabled pulldown=disabled active_low=true wiring=GPIO%d-to-GND",
             BUTTON_GPIO,
             BUTTON_GPIO);
}

void init_led()
{
    led_strip_config_t strip_config = {};
    strip_config.strip_gpio_num = LED_GPIO;
    strip_config.max_leds = LED_COUNT;
    strip_config.led_model = LED_MODEL_WS2812;
    strip_config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;

    led_strip_rmt_config_t rmt_config = {};
    rmt_config.resolution_hz = 10 * 1000 * 1000;
    rmt_config.flags.with_dma = false;

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &g_led_strip));
    hal_led_clear();
}

void webserver_task(void *)
{
    start_http_server();
    while (true) {
        xEventGroupWaitBits(g_wifi_event_group, kWifiConnectedBit, pdFALSE, pdTRUE, pdMS_TO_TICKS(1000));

        state_ctrl_t ctrl = {};
        state_ctrl_read(&ctrl);

        state_desr_t desired = {};
        state_desr_read(&desired);
        if (desired.command_pending && desired.command_seq == ctrl.command_seq_seen) {
            desired.command_pending = false;
            state_desr_write(desired);
        }

        if (g_wifi_connected.load() && ctrl.event_pending && post_pending_event(ctrl)) {
            state_desr_read(&desired);
            desired.event_ack_pending = true;
            desired.event_ack_seq = ctrl.event_seq;
            state_desr_write(desired);
        }

        vTaskDelay(pdMS_TO_TICKS(COMMS_PERIOD_MS));
    }
}

void sense_button()
{
    const int required_stable_samples = kDebounceStableMs / SENSE_PERIOD_MS;
    bool last_raw_pressed = hal_read_button_gpio_level() == 0;
    bool debounced_pressed = last_raw_pressed;
    int stable_samples = 0;

    while (true) {
        const bool raw_pressed = hal_read_button_gpio_level() == 0;
        if (raw_pressed == last_raw_pressed) {
            ++stable_samples;
        } else {
            last_raw_pressed = raw_pressed;
            stable_samples = 1;
        }

        bool fell = false;
        bool rose = false;
        if (stable_samples >= required_stable_samples && raw_pressed != debounced_pressed) {
            fell = raw_pressed;
            rose = !raw_pressed;
            debounced_pressed = raw_pressed;
            ESP_LOGI(TAG,
                     "button transition event=%s gpio=%d raw_level=%d pressed=%s uptime_ms=%u",
                     fell ? "button_press" : "button_release",
                     BUTTON_GPIO,
                     raw_pressed ? 0 : 1,
                     debounced_pressed ? "true" : "false",
                     uptime_ms());
        }

        state_sens_t state = {};
        state.kind = sensor_kind_t::button;
        state.button.pressed = debounced_pressed;
        state.button.fell = fell;
        state.button.rose = rose;
        state.button.tstamp_ms = uptime_ms();
        state_sens_write(state);

        vTaskDelay(pdMS_TO_TICKS(SENSE_PERIOD_MS));
    }
}

void sense_rebounder()
{
    mpu_t mpu = {};
    bool over_threshold = false;
    uint32_t next_retry_ms = 0;
    uint32_t next_log_ms = 0;
    bool read_failure_logged = false;

    while (true) {
        if (g_reset_sensor_filter.exchange(false)) {
            over_threshold = false;
        }

        const uint32_t now_ms = uptime_ms();
        if (!mpu.initialized) {
            if (static_cast<int32_t>(now_ms - next_retry_ms) >= 0) {
                const esp_err_t err = mpu_init(mpu);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "MPU6050 init failed: %s", esp_err_to_name(err));
                    next_retry_ms = now_ms + kMpuRetryMs;
                } else {
                    over_threshold = false;
                    read_failure_logged = false;
                    next_log_ms = now_ms + ACCEL_LOG_PERIOD_MS;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(SENSE_PERIOD_MS));
            continue;
        }

        axis_sample_t raw_sample = {};
        const esp_err_t err = hal_read_mpu_accel(&raw_sample, mpu);
        if (err != ESP_OK) {
            if (!read_failure_logged) {
                ESP_LOGW(TAG, "MPU6050 read failed: %s", esp_err_to_name(err));
                read_failure_logged = true;
            }
            mpu_deinit(mpu);
            next_retry_ms = now_ms + kMpuRetryMs;
            over_threshold = false;
            vTaskDelay(pdMS_TO_TICKS(SENSE_PERIOD_MS));
            continue;
        }

        if (read_failure_logged) {
            ESP_LOGI(TAG, "MPU6050 read recovered");
            read_failure_logged = false;
        }

        const float magnitude_g = sqrtf(raw_sample.x * raw_sample.x + raw_sample.y * raw_sample.y + raw_sample.z * raw_sample.z);
        const bool is_over_threshold = magnitude_g > ACCEL_THRESHOLD_G;
        const bool rising_edge = !over_threshold && is_over_threshold;
        over_threshold = is_over_threshold;

        state_sens_t state = {};
        state.kind = sensor_kind_t::rebounder;
        state.rebounder.magnitude_g = magnitude_g;
        state.rebounder.rising_edge = rising_edge;
        state.rebounder.tstamp_ms = now_ms;
        state_sens_write(state);

        if (ACCEL_LOG_PERIOD_MS > 0 && static_cast<int32_t>(now_ms - next_log_ms) >= 0) {
            ESP_LOGI(TAG,
                     "accel t=%u raw=%.3f,%.3f,%.3f mag=%.3f over=%s edge=%s",
                     now_ms,
                     raw_sample.x,
                     raw_sample.y,
                     raw_sample.z,
                     magnitude_g,
                     is_over_threshold ? "1" : "0",
                     rising_edge ? "1" : "0");
            next_log_ms = now_ms + ACCEL_LOG_PERIOD_MS;
        }

        vTaskDelay(pdMS_TO_TICKS(SENSE_PERIOD_MS));
    }
}

void sense_task(void *)
{
    if (module_is_button()) {
        sense_button();
    } else {
        sense_rebounder();
    }
}

void queue_triggered_event(state_ctrl_t *ctrl)
{
    if (ctrl->event_pending) {
        return;
    }
    ctrl->event_pending = true;
    ctrl->event_seq += 1;
    strlcpy(ctrl->event, "triggered", sizeof(ctrl->event));
}

void apply_command(command_t command, state_ctrl_t *ctrl, uint32_t *inactive_flash_until_ms, uint32_t *inactive_ignore_until_ms)
{
    if (command == command_t::activate) {
        if (ctrl->triggered) {
            ESP_LOGW(TAG, "activate ignored: already_triggered");
            return;
        }
        ctrl->active = true;
        *inactive_flash_until_ms = 0;
        if (module_is_rebounder()) {
            g_reset_sensor_filter.store(true);
        }
    } else if (command == command_t::deactivate) {
        ctrl->active = false;
        ctrl->triggered = false;
        ctrl->event_pending = false;
        *inactive_flash_until_ms = 0;
        *inactive_ignore_until_ms = module_is_rebounder() ? uptime_ms() + DEACTIVATE_IGNORE_MS : 0;
    }
}

void control_task(void *)
{
    state_ctrl_t ctrl = {};
    uint32_t last_command_seq = 0;
    uint32_t inactive_flash_until_ms = 0;
    uint32_t inactive_ignore_until_ms = 0;
    bool led_flash_on = false;

    while (true) {
        state_desr_t desired = {};
        state_desr_read(&desired);
        if (desired.command_pending && desired.command_seq != last_command_seq) {
            apply_command(desired.command, &ctrl, &inactive_flash_until_ms, &inactive_ignore_until_ms);
            last_command_seq = desired.command_seq;
            ctrl.command_seq_seen = last_command_seq;
        }
        if (desired.event_ack_pending && desired.event_ack_seq == ctrl.event_seq) {
            ctrl.event_pending = false;
        }

        state_sens_t sensed = {};
        state_sens_read(&sensed);

        const uint32_t now_ms = uptime_ms();
        bool sensed_trigger = false;
        if (sensed.kind == sensor_kind_t::button) {
            sensed_trigger = sensed.button.fell;
        } else {
            sensed_trigger = sensed.rebounder.rising_edge;
        }

        if (sensed_trigger) {
            if (ctrl.active) {
                if (!ctrl.triggered) {
                    ctrl.triggered = true;
                    queue_triggered_event(&ctrl);
                }
            } else if (sensed.kind == sensor_kind_t::button || static_cast<int32_t>(now_ms - inactive_ignore_until_ms) >= 0) {
                ctrl.triggered = true;
                inactive_flash_until_ms = now_ms + kFlashDurationMs;
                ESP_LOGI(TAG, "inactive trigger module=%s uptime_ms=%u", kModuleName, now_ms);
            }
        }

        if (!ctrl.active && ctrl.triggered && inactive_flash_until_ms != 0 && static_cast<int32_t>(now_ms - inactive_flash_until_ms) >= 0) {
            ctrl.triggered = false;
            inactive_flash_until_ms = 0;
        }

        if (ctrl.active && !ctrl.triggered) {
            hal_led_set_rgb(0, 0, 32);
        } else if (!ctrl.active && ctrl.triggered && inactive_flash_until_ms != 0) {
            led_flash_on = !led_flash_on;
            if (led_flash_on) {
                hal_led_set_rgb(32, 0, 0);
            } else {
                hal_led_clear();
            }
        } else {
            hal_led_clear();
        }

        state_ctrl_write(ctrl);
        vTaskDelay(pdMS_TO_TICKS(CONTROL_PERIOD_MS));
    }
}

} // namespace

void module_app_start()
{
    g_desr_mutex = xSemaphoreCreateMutex();
    g_sens_mutex = xSemaphoreCreateMutex();
    g_ctrl_mutex = xSemaphoreCreateMutex();

    ESP_ERROR_CHECK(g_desr_mutex == nullptr ? ESP_ERR_NO_MEM : ESP_OK);
    ESP_ERROR_CHECK(g_sens_mutex == nullptr ? ESP_ERR_NO_MEM : ESP_OK);
    ESP_ERROR_CHECK(g_ctrl_mutex == nullptr ? ESP_ERR_NO_MEM : ESP_OK);

    if (!module_is_button() && !module_is_rebounder()) {
        ESP_LOGE(TAG, "invalid MODULE_TYPE: %s", kModuleType);
        abort();
    }

    g_sens.kind = module_is_button() ? sensor_kind_t::button : sensor_kind_t::rebounder;
    if (module_is_button()) {
        init_button();
    }
    init_led();
    init_wifi();

    xTaskCreate(webserver_task, "webserver", 8192, nullptr, 4, nullptr);
    xTaskCreate(sense_task, "sense", 4096, nullptr, 5, nullptr);
    xTaskCreate(control_task, "control", 4096, nullptr, 5, nullptr);
}
