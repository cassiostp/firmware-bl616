// Host sim shim for Bouffalo bl616_glb.h.
#pragma once

#include <stdint.h>
#include "bflb_mtimer.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register reads return 0 on the sim (used only by print_system_info).
#define BL_RD_WORD(addr) ((void)(addr), 0u)

void GLB_SW_System_Reset(void);
void arch_delay_ms(uint32_t ms);

#ifdef __cplusplus
}
#endif
