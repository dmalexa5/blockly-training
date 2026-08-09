#include "protocol.hpp"
#include "webserver.hpp"
#include "sense.hpp"
#include "control.hpp"

extern "C" void app_main(void)
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
