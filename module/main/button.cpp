#include "button.hpp"

#include <cstdio>
#include <cstring>

#include "driver/gpio.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

namespace {

constexpr char kTag[] = "button";
constexpr int kDebounceStableMs = 50;
constexpr int kFlashDurationMs = 1000;
constexpr int kHttpTimeoutMs = 1000;

led_strip_handle_t g_led_strip = nullptr;

uint32_t uptime_ms()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

bool read_pending_command(ButtonCommand *command, uint32_t *command_seq)
{
    // TODO: Read this from Comms once main.cpp owns the object wiring.
    *command = ButtonCommand::none;
    *command_seq = 0;
    return false;
}

bool read_event_ack(uint32_t *event_seq)
{
    // TODO: Read this from Comms once main.cpp owns the object wiring.
    *event_seq = 0;
    return false;
}

bool read_button_trigger()
{
    // TODO: Read this from Sense once main.cpp owns the object wiring.
    return false;
}

bool read_control_event(ButtonControlState *control)
{
    // TODO: Read this from Control once main.cpp owns the object wiring.
    *control = {};
    return false;
}

void queue_triggered_event(ButtonControlState *control)
{
    if (control->event_pending) {
        return;
    }

    control->event_pending = true;
    control->event_seq += 1;
    std::snprintf(control->event, sizeof(control->event), "triggered");
}

void apply_command(ButtonCommand command, ButtonControlState *control, uint32_t *inactive_flash_until_ms)
{
    if (command == ButtonCommand::activate) {
        if (control->triggered) {
            ESP_LOGW(kTag, "activate ignored: already_triggered");
            return;
        }
        control->active = true;
        *inactive_flash_until_ms = 0;
    } else if (command == ButtonCommand::deactivate) {
        control->active = false;
        control->triggered = false;
        control->event_pending = false;
        *inactive_flash_until_ms = 0;
    }
}

void led_set_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    if (g_led_strip == nullptr) {
        return;
    }

    led_strip_set_pixel(g_led_strip, 0, red, green, blue);
    led_strip_refresh(g_led_strip);
}

void led_clear()
{
    if (g_led_strip != nullptr) {
        led_strip_clear(g_led_strip);
    }
}

bool parse_command(const char *json, ButtonCommand *command)
{
    if (json == nullptr) {
        return false;
    }
    if (std::strstr(json, "\"cmd\"") != nullptr && std::strstr(json, "\"activate\"") != nullptr) {
        *command = ButtonCommand::activate;
        return true;
    }
    if (std::strstr(json, "\"cmd\"") != nullptr && std::strstr(json, "\"deactivate\"") != nullptr) {
        *command = ButtonCommand::deactivate;
        return true;
    }
    return false;
}

bool post_event(const ButtonControlState &control)
{
    char json[96];
    const int written = std::snprintf(json, sizeof(json), "{\"event\":\"%s\"}", control.event);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(json)) {
        return false;
    }

    esp_http_client_config_t config = {};
    config.url = SERVER_BASE_URL "/events";
    config.timeout_ms = kHttpTimeoutMs;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return false;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json, std::strlen(json));
    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    return err == ESP_OK && status >= 200 && status < 300;
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

    ButtonCommand command = ButtonCommand::none;
    if (!parse_command(body, &command)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid command");
        return ESP_OK;
    }

    // TODO: Publish command into Comms state.
    httpd_resp_set_status(req, "202 Accepted");
    return httpd_resp_send(req, nullptr, 0);
}

} // namespace

void Sense::init()
{
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = 1ULL << BUTTON_GPIO;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io_conf);
}

void Sense::task()
{
    const int required_stable_samples = kDebounceStableMs / SENSE_PERIOD_MS;
    bool last_raw_pressed = gpio_get_level(static_cast<gpio_num_t>(BUTTON_GPIO)) == 0;
    bool debounced_pressed = last_raw_pressed;
    int stable_samples = 0;

    while (true) {
        const bool raw_pressed = gpio_get_level(static_cast<gpio_num_t>(BUTTON_GPIO)) == 0;
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
            ESP_LOGI(kTag, "button transition event=%s uptime_ms=%u", fell ? "press" : "release", uptime_ms());
        }

        ButtonSenseState state = {};
        state.pressed = debounced_pressed;
        state.fell = fell;
        state.rose = rose;
        state.tstamp_ms = uptime_ms();
        write_state(state);

        vTaskDelay(pdMS_TO_TICKS(SENSE_PERIOD_MS));
    }
}

void Sense::deinit()
{
}

void Control::init()
{
    ButtonControlState state = {};
    write_state(state);
}

void Control::task()
{
    ButtonControlState control = {};
    uint32_t last_command_seq = 0;
    uint32_t inactive_flash_until_ms = 0;
    bool led_flash_on = false;

    while (true) {
        ButtonCommand command = ButtonCommand::none;
        uint32_t command_seq = 0;
        if (read_pending_command(&command, &command_seq) && command_seq != last_command_seq) {
            apply_command(command, &control, &inactive_flash_until_ms);
            last_command_seq = command_seq;
            control.command_seq_seen = last_command_seq;
        }

        uint32_t ack_event_seq = 0;
        if (read_event_ack(&ack_event_seq) && ack_event_seq == control.event_seq) {
            control.event_pending = false;
        }

        const uint32_t now_ms = uptime_ms();
        if (read_button_trigger()) {
            if (control.active) {
                if (!control.triggered) {
                    control.triggered = true;
                    queue_triggered_event(&control);
                }
            } else {
                control.triggered = true;
                inactive_flash_until_ms = now_ms + kFlashDurationMs;
                ESP_LOGI(kTag, "inactive trigger uptime_ms=%u", now_ms);
            }
        }

        if (!control.active && control.triggered && inactive_flash_until_ms != 0 &&
            static_cast<int32_t>(now_ms - inactive_flash_until_ms) >= 0) {
            control.triggered = false;
            inactive_flash_until_ms = 0;
        }

        if (control.active && !control.triggered) {
            led_set_rgb(0, 0, 32);
        } else if (!control.active && control.triggered && inactive_flash_until_ms != 0) {
            led_flash_on = !led_flash_on;
            led_flash_on ? led_set_rgb(32, 0, 0) : led_clear();
        } else {
            led_clear();
        }

        write_state(control);
        vTaskDelay(pdMS_TO_TICKS(CONTROL_PERIOD_MS));
    }
}

void Control::deinit()
{
    led_clear();
}

void Comms::init()
{
    ButtonCommsState state = {};
    write_state(state);

    // TODO: Start Wi-Fi and register HTTP handlers once networking ownership is split out.
}

void Comms::task()
{
    while (true) {
        ButtonControlState control = {};
        if (read_control_event(&control) && control.event_pending && post_event(control)) {
            ButtonCommsState state = {};
            read_state(&state);
            state.event_ack_pending = true;
            state.event_ack_seq = control.event_seq;
            write_state(state);
        }

        vTaskDelay(pdMS_TO_TICKS(COMMS_PERIOD_MS));
    }
}

void Comms::deinit()
{
}
