#pragma once

#include "interfaces.hpp"

#define MPU_SDA_GPIO 8 
#define MPU_SCL_GPIO 9 
#define MPU_I2C_ADDR 0x68

#define ACCEL_THRESHOLD_G 1.2 
#define DEACTIVATE_IGNORE_MS 100 
#define ACCEL_LOG_PERIOD_MS 20 

class RebounderControl : ControlInterface {
    State state {

    };
};
