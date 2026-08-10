#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define COMMS_PERIOD_MS 50
#define SENSE_PERIOD_MS 1
#define CONTROL_PERIOD_MS 10


template <typename Derived>
class SenseInterface {
public:
    using State = typename Derived::State;

    //TODO: Rewrite this so that it is correct with the templated/child implemented state
    inline void read_state(State *out) {
        xSemaphoreTake(mutex, portMAX_DELAY);
        *out = state;
        xSemaphoreGive(mutex);
    }

    virtual void init(); 
    virtual void task();
    virtual void deinit();

protected:
    // TODO: Rewrite so that this is correct with the templated/child implemented State
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
