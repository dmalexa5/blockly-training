#pragma once

#include <cstdint>
#include <atomic>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"


#define COMMS_PERIOD_MS 50
#define SENSE_PERIOD_MS 1
#define CONTROL_PERIOD_MS 10

template <typename StateT>
class SenseInterface {
public:
    using State = StateT;

    SenseInterface() = default;
    virtual ~SenseInterface() = default;

    void read_state(State *out)
    {
        xSemaphoreTake(mutex, portMAX_DELAY);
        *out = state;
        xSemaphoreGive(mutex);
    }

    virtual void init() = 0;
    virtual void task(void *) = 0;
    virtual void deinit() = 0;

    bool read_flag() 
    {
        return flag.load();
    }

    void clear_flag()
    {
        flag.store(true);
    }

protected:
    void write_state(const State &in)
    {
        xSemaphoreTake(mutex, portMAX_DELAY);
        state = in;
        xSemaphoreGive(mutex);
    }
    void set_flag()
    {
        flag.store(true);
    }

private:
    SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    State state = {};
    
    std::atomic<bool> flag{false};

};

template <typename StateT>
class ControlInterface {
public:
    using State = StateT;

    ControlInterface() = default;
    virtual ~ControlInterface() = default;

    void read_state(State *out)
    {
        xSemaphoreTake(mutex, portMAX_DELAY);
        *out = state;
        xSemaphoreGive(mutex);
    }

    virtual void init() = 0;
    virtual void task(void *) = 0;
    virtual void deinit() = 0;

protected:
    void write_state(const State &in)
    {
        xSemaphoreTake(mutex, portMAX_DELAY);
        state = in;
        xSemaphoreGive(mutex);
    }

private:
    SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    State state = {};
};

template <typename StateT>
class CommsInterface {
public:
    using State = StateT;

    CommsInterface() = default;
    virtual ~CommsInterface() = default;

    void read_state(State *out)
    {
        xSemaphoreTake(mutex, portMAX_DELAY);
        *out = state;
        xSemaphoreGive(mutex);
    }

    virtual void init() = 0;
    virtual void task(void *) = 0;
    virtual void deinit() = 0;

protected:
    void write_state(const State &in)
    {
        xSemaphoreTake(mutex, portMAX_DELAY);
        state = in;
        xSemaphoreGive(mutex);
    }

private:
    SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    State state = {};
};
