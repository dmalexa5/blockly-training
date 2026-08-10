#pragma once

#include <cstdint>

#include "freertos/FreeRTOS.h"


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

struct state_sense_t {
    sensor_kind_t kind;
    union {
        button_sens_t button;
        rebounder_sens_t rebounder;
    };
};

struct state_desr_t {
    command_t command;
    bool command_pending;
    uint32_t command_seq;
    bool event_ack_pending;
    uint32_t event_ack_seq;
};


struct state_ctrl_t {
    bool active;
    bool triggered;
    bool event_pending;
    uint32_t event_seq;
    uint32_t command_seq_seen;
    char event[24];
};


inline bool module_is_button()
{
    return strcmp(MODULE_TYPE, "button") == 0;
}

inline bool module_is_rebounder()
{
    return strcmp(MODULE_TYPE, "rebounder") == 0;
}
