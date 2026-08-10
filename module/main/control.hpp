#pragma once

#include "protocol.hpp"

void led_set_rgb(uint8_t red, uint8_t green, uint8_t blue);
void led_clear();
void led_init();

void control_init();
void control_task(void *);

