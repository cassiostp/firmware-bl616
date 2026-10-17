// Host sim shim for Bouffalo bl616_glb.h.
#pragma once

#include <stdint.h>
#include "bflb_mtimer.h"

#ifdef __cplusplus
extern "C" {
#endif

// Register reads return 0 on the sim (used only by print_system_info).
#define BL_RD_WORD(addr) ((void)(addr), 0u)

typedef struct {
    uint8_t reset_recorder_ana_powb : 1;
    uint8_t reset_recorder_ext_rst_n : 1;
    uint8_t reset_recorder_pds_reset : 1;
    uint8_t reset_recorder_wdt_rst_n : 1;
    uint8_t reset_recorder_cpu_porst_n : 1;
    uint8_t reset_recorder_sys_reset_n : 1;
    uint8_t reset_recorder_cpu_sys_rstreq_n : 1;
    uint8_t reset_recorder_rsvd : 1;
} GLB_RESET_RECORD_Type;
int GLB_Get_Reset_Reason(GLB_RESET_RECORD_Type *reason);

void GLB_SW_System_Reset(void);
void arch_delay_ms(uint32_t ms);

#ifdef __cplusplus
}
#endif
