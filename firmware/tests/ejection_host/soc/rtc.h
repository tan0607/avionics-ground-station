#pragma once
#include <cstdint>
inline uint64_t hostRtcOffsetUs = 0;
extern unsigned long hostNowMs;
inline uint64_t rtc_time_get() { return hostRtcOffsetUs + hostNowMs * 1000ULL; }
inline uint64_t rtc_time_slowclk_to_us(uint64_t ticks, uint32_t cal) { return (ticks * cal) >> 19; }
