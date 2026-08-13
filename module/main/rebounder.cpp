#include "rebounder.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

namespace {

// constants
constexpr char kTag[] = "rebounder";
constexpr int kFlashDurationMs = 1000;
constexpr int kHttpTimeoutMs = 1000;
constexpr int kMpuI2cTimeoutMs = 100;
constexpr int kMpuRetryMs = 1000;
constexpr float kAccelLsbPerG = 8192.0f;

constexpr uint8_t kMpuRegAccelConfig = 0x1C;
constexpr uint8_t kMpuRegAccelXoutH = 0x3B;
constexpr uint8_t kMpuRegConfig = 0x1A;
constexpr uint8_t kMpuRegPwrMgmt1 = 0x6B;
constexpr uint8_t kMpuRegSampleRateDiv = 0x19;
constexpr uint8_t kMpuRegWhoAmI = 0x75;

// util
// TODO: define utility functions here

} // namespace

void Sense::init()
{
    RebounderSenseState state = {};
    write_state(state);
}

void Sense::task()
{
    while (true) {
       
       // read debug state

       // if mpu not initialize
            // attempt initialization
            // if successful, 
                // log info if debug state sense task log level >= info
                // continue
            // else
                // log error if debug state sense task log level >= err
        
        // read accelerometer magnitude

        // if mag > threshold, flag is false, current time - last event time > debounce period
            // set flag true
            // last event time = current time

        //if debug state sense task data = true
            // log accelerometer x y z and magnitude 
            // log flag

        // if debug state sense task timing = true
            // log time for task to complete
        // log error if task time > SENSE_PERIOD_MS
        // vTaskDelay SENSE_PERIOD_MS - task time

        vTaskDelay(pdMS_TO_TICKS(SENSE_PERIOD_MS));
    }
}

void Sense::deinit()
{
}

void Control::init()
{
    RebounderControlState state = {};
    write_state(state);
}

void Control::task()
{

    while(true) {

     
        // read debug state
        // read sense state
        // read comms state

        // if sense 
        
        // 
        

        
        // if debug state sense task timing = true
            // log time for task to complete
        // log error if task time > SENSE_PERIOD_MS
        // vTaskDelay SENSE_PERIOD_MS - task time
    
    
    
    }
}

void Control::deinit()
{
    led_clear();
}

void Comms::init()
{
    RebounderCommsState state = {};
    write_state(state);

    // TODO: Start Wi-Fi and register HTTP handlers once networking ownership is split out.
}

void Comms::task()
{
    while (true) {
        RebounderControlState control = {};
        if (read_control_event(&control) && control.event_pending && post_event(control)) {
            RebounderCommsState state = {};
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
