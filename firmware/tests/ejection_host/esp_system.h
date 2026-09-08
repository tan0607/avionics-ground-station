#pragma once
#define RTC_DATA_ATTR
// Preserve the actual retained bytes between separate host processes. This
// models retention/loss explicitly; it does not emulate physical RTC power.
#ifdef __APPLE__
#define RTC_NOINIT_ATTR __attribute__((section("__DATA,mrcc_rtc")))
#else
#define RTC_NOINIT_ATTR __attribute__((section("mrcc_rtc")))
#endif
#define ESP_RST_POWERON 1
#define ESP_RST_SW 3
inline int hostResetReason = ESP_RST_SW;
inline int esp_reset_reason() { return hostResetReason; }
