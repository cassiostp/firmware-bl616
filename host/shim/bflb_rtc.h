// Host sim shim for Bouffalo bflb_rtc.h (HBN RTC counter). Matches the real
// SDK header: counter read only, no init entry point.
#pragma once

#include <stdint.h>

#include "bflb_device.h"

#ifdef __cplusplus
extern "C" {
#endif

uint64_t bflb_rtc_get_time(struct bflb_device_s *dev);

#ifdef __cplusplus
}
#endif
