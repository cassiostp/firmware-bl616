// Host sim clock declarations (covers Bouffalo bflb_clock.h/bl616_clock.h).
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    BL_SYSTEM_CLOCK_MCU_CLK = 0,
};

enum {
    BL_PERIPHERAL_CLOCK_UART0 = 0,
    BL_PERIPHERAL_CLOCK_UART1 = 1,
};

uint32_t bflb_clk_get_system_clock(int type);
uint32_t Clock_Peripheral_Clock_Get(int type);

#ifdef __cplusplus
}
#endif
