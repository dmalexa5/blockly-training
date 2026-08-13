#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef MODULE_TYPE
#error "MODULE_TYPE must be defined"
#endif

#if MODULE_TYPE == 1
#include "button.hpp"
#elif MODULE_TYPE == 2
#include "rebounder.hpp"
#else
#error "Unsupported MODULE_TYPE"
#endif

namespace {
    Comms comms;
    Sense sense;
    Control control;
}
extern "C" void app_main(void)
{
    comms.init();
    sense.init();
    control.init();

    xTaskCreate(comms.task, "comms", 8192, nullptr, 4, nullptr);
    xTaskCreate(sense.task, "sense", 4096, nullptr, 5, nullptr);
    xTaskCreate(control.task, "control", 4096, nullptr, 5, nullptr);
}
