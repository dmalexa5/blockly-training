#pragma once

#define COMMS_PERIOD_MS 50
#define SENSE_PERIOD_MS 1
#define CONTROL_PERIOD_MS 10

#if MODULE_TYPE "button"
#define BUTTON_GPIO 4 
#define LED_GPIO 48 
#define LED_COUNT 1 
#elseif MODULE_TYPE "rebounder"
#define MPU_SDA_GPIO 8 
#define MPU_SCL_GPIO 9 
#define MPU_I2C_ADDR 0x68

#define ACCEL_THRESHOLD_G 1.2 
#define DEACTIVATE_IGNORE_MS 100 
#define ACCEL_LOG_PERIOD_MS 20 

#else
// TODO: some sort of mock mode here?
#endif
