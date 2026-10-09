// Host sim shim for Bouffalo bflb_mtimer.h (wall-clock time).
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint64_t bflb_mtimer_get_time_ms(void);
uint64_t bflb_mtimer_get_time_us(void);

#ifdef __cplusplus
}
#endif
