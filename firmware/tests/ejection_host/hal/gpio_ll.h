#pragma once
#include "Arduino.h"
inline void gpio_ll_set_level(int*, int pin, int level) { digitalWrite(pin, level); }
