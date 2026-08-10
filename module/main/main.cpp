#include "protocol.hpp"
#include "webserver.hpp"
#include "sense.hpp"
#include "control.hpp"

CommsInterface comms;
SenseInterface sense;
ControlInterface control;

extern "C" void app_main(void)
{
    g_desr_mutex = xSemaphoreCreateMutex();
    g_sens_mutex = xSemaphoreCreateMutex();
    g_ctrl_mutex = xSemaphoreCreateMutex();

    ESP_ERROR_CHECK(g_desr_mutex == nullptr ? ESP_ERR_NO_MEM : ESP_OK);
    ESP_ERROR_CHECK(g_sens_mutex == nullptr ? ESP_ERR_NO_MEM : ESP_OK);
    ESP_ERROR_CHECK(g_ctrl_mutex == nullptr ? ESP_ERR_NO_MEM : ESP_OK);

    xTaskCreate(webserver_task, "webserver", 8192, nullptr, 4, nullptr);
    xTaskCreate(sense_task, "sense", 4096, nullptr, 5, nullptr);
    xTaskCreate(control_task, "control", 4096, nullptr, 5, nullptr);
}
