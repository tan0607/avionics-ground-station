#pragma once
#include <cstdint>
using esp_err_t = int;
constexpr int ESP_OK = 0;
constexpr int ESP_FAIL = -1;
constexpr int GPTIMER_CLK_SRC_DEFAULT = 0;
constexpr int GPTIMER_COUNT_UP = 0;
struct host_timer;
using gptimer_handle_t = host_timer*;
struct gptimer_alarm_event_data_t {};
struct gptimer_config_t { int clk_src; int direction; uint32_t resolution_hz; int intr_priority; };
struct gptimer_alarm_config_t { uint64_t alarm_count; uint64_t reload_count; struct { bool auto_reload_on_alarm; } flags; };
struct gptimer_event_callbacks_t { bool (*on_alarm)(gptimer_handle_t, const gptimer_alarm_event_data_t*, void*); };
esp_err_t gptimer_new_timer(const gptimer_config_t*, gptimer_handle_t*);
esp_err_t gptimer_register_event_callbacks(gptimer_handle_t, const gptimer_event_callbacks_t*, void*);
esp_err_t gptimer_enable(gptimer_handle_t);
esp_err_t gptimer_disable(gptimer_handle_t);
esp_err_t gptimer_del_timer(gptimer_handle_t);
esp_err_t gptimer_stop(gptimer_handle_t);
esp_err_t gptimer_start(gptimer_handle_t);
esp_err_t gptimer_set_raw_count(gptimer_handle_t, uint64_t);
esp_err_t gptimer_set_alarm_action(gptimer_handle_t, const gptimer_alarm_config_t*);
