#pragma once
// State.cpp's real latch/checksum runs on host RAM. Persistence across actual
// ESP32 reset classes is NOT emulated by removing this placement attribute.
#define RTC_DATA_ATTR
