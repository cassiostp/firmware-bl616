// Host sim shim for Bouffalo bflb_rtc.h (HBN RTC counter).
#pragma once

#include <stdint.h>

#include "bflb_device.h"

#ifdef __cplusplus
extern "C" {
#endif

enum bflb_rtc_32k_clk_type {
    BFLB_RTC_32K_CLK_RC = 0,
    BFLB_RTC_32K_CLK_XTAL,
    BFLB_RTC_32K_CLK_DIG = 3,
};

void bflb_rtc_init(struct bflb_device_s *dev, enum bflb_rtc_32k_clk_type clk_type);
uint64_t bflb_rtc_get_time(struct bflb_device_s *dev);

#ifdef __cplusplus
}
#endif
