#pragma once
#include <cstdint>
inline uint32_t hostCalibration = 1U << 19;
inline uint32_t esp_clk_slowclk_cal_get() { return hostCalibration; }
