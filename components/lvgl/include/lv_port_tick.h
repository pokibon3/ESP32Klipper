// Millisecond tick for LVGL (LV_TICK_CUSTOM)
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
uint32_t lv_port_tick_ms(void);
#ifdef __cplusplus
}
#endif
