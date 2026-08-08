#include "app.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cinttypes>
#include <cstring>

#include "driver/gpio.h"
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

#ifndef RBDR_BUTTON_GPIO
#define RBDR_BUTTON_GPIO 4
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

namespace {

constexpr char kModuleName[] = "button";
constexpr int kCommandQueueLen = 8;
constexpr int kHttpTimeoutMs = 1000;
constexpr int kDebounceStableMs = 50;
constexpr int kFlashDurationMs = 1000;
constexpr EventBits_t kWifiConnectedBit = BIT0;

const char *TAG = "rebounder";

enum class command_t {
    activate,
    deactivate,
};

struct mod_state_t {
    bool pressed;
    bool fell;
    bool rose;
    uint32_t tstamp_ms;
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
    snprintf(url, sizeof(url), "%s/poll", RBDR_SERVER_BASE_URL);

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

void init_button()
{
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = 1ULL << RBDR_BUTTON_GPIO;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&io_conf));
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
    const int required_stable_samples = kDebounceStableMs / RBDR_SENSE_PERIOD_MS;
    bool last_raw_pressed = gpio_get_level(static_cast<gpio_num_t>(RBDR_BUTTON_GPIO)) == 0;
    bool debounced_pressed = last_raw_pressed;
    int stable_samples = 0;

    while (true) {
        const bool raw_pressed = gpio_get_level(static_cast<gpio_num_t>(RBDR_BUTTON_GPIO)) == 0;
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
        }

        mod_state_t state = {};
        state.pressed = debounced_pressed;
        state.fell = fell;
        state.rose = rose;
        state.tstamp_ms = uptime_ms();

        xSemaphoreTake(g_state_mutex, portMAX_DELAY);
        g_latest_state = state;
        xSemaphoreGive(g_state_mutex);

        vTaskDelay(pdMS_TO_TICKS(RBDR_SENSE_PERIOD_MS));
    }
}

void control_task(void *)
{
    bool active = false;
    bool triggered = false;
    uint32_t inactive_flash_until_ms = 0;
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
                }
            } else if (command == command_t::deactivate) {
                active = false;
                triggered = false;
                inactive_flash_until_ms = 0;
            }
        }

        mod_state_t state = {};
        xSemaphoreTake(g_state_mutex, portMAX_DELAY);
        state = g_latest_state;
        xSemaphoreGive(g_state_mutex);

        if (state.fell) {
            if (active) {
                if (!triggered) {
                    triggered = true;
                    write_event("triggered", nullptr, active, triggered);
                }
            } else {
                triggered = true;
                inactive_flash_until_ms = uptime_ms() + kFlashDurationMs;
            }
        }

        const uint32_t now_ms = uptime_ms();
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

    init_button();
    init_led();
    init_wifi();

    xTaskCreate(comms_task, "comms_task", 6144, nullptr, 4, &g_comms_task_handle);
    xTaskCreate(sense_task, "sense_task", 2048, nullptr, 5, nullptr);
    xTaskCreate(control_task, "control_task", 4096, nullptr, 5, nullptr);
}
