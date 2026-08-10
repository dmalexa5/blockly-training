#pragma once

#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"



class SenseInterface {
public:
    SenseInterface() = default;

    struct State;

    inline void read_state(State *out) {
        xSemaphoreTake(mutex, portMAX_DELAY);
        *out = state;
        xSemaphoreGive(mutex);
    }

    virtual void init(); 
    virtual void task();
    virtual void deinit();

protected:
    inline void write_state(const State &in) {
        xSemaphoreTake(mutex, portMAX_DELAY);
        state = in;
        xSemaphoreGive(mutex);
    }

private:
    SemaphoreHandle_t mutex = nullptr;
    State state;
};

class ControlInterface {
public:
    ControlInterface() = default;

    struct State;

    inline void read_state(State *out) {
        xSemaphoreTake(mutex, portMAX_DELAY);
        *out = state;
        xSemaphoreGive(mutex);
    }

    virtual void init();
    virtual void task();
    virtual void deinit();

protected:
    inline void write_state(const State &in) {
        xSemaphoreTake(mutex, portMAX_DELAY);
        state = in;
        xSemaphoreGive(mutex);
    }

private:
    SemaphoreHandle_t mutex = nullptr;
    State state;
};

class CommsInterface {
public:
    CommsInterface() = default;

    struct State;

    inline void read_state(State *out) {
        xSemaphoreTake(mutex, portMAX_DELAY);
        *out = state;
        xSemaphoreGive(mutex);
    }

    virtual void init();
    virtual void task();
    virtual void deinit();

protected:
    inline void write_state(const State &in) {
        xSemaphoreTake(mutex, portMAX_DELAY);
        state = in;
        xSemaphoreGive(mutex);
    }

private:
    SemaphoreHandle_t mutex = nullptr;
    State state;
};
