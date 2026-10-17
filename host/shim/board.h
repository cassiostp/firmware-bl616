// Host sim shim for the board support package.
#pragma once

#include "bflb_mtimer.h"

#ifdef __cplusplus
extern "C" {
#endif

void board_init(void);
void board_sdh_gpio_init(void);

#ifdef __cplusplus
}
#endif
