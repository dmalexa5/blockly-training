#pragma once

#include <cstdint>

#include "interfaces.hpp"

enum class RebounderCommand {
    none,
    activate,
    deactivate,
};

struct RebounderSenseState {
    float magnitude;
    bool rising;
    bool falling;
    uint32_t tstamp;
};

struct RebounderControlState {
    bool active;
    bool triggered;
    bool event_pending;
    uint32_t event_seq;
    uint32_t command_seq_seen;
    char event[24];
};

struct RebounderCommsState {
    RebounderCommand command;
    bool command_pending;
    uint32_t command_seq;
    bool event_ack_pending;
    uint32_t event_ack_seq;
};

class Sense : public SenseInterface<RebounderSenseState> {
public:
    void init() override;
    void task() override;
    void deinit() override;
};

class Control : public ControlInterface<RebounderControlState> {
public:
    void init() override;
    void task() override;
    void deinit() override;
};

class Comms : public CommsInterface<RebounderCommsState> {
public:
    void init() override;
    void task() override;
    void deinit() override;
};
