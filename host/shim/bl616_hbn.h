// Host sim shim for Bouffalo bl616_hbn.h.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint8_t HBN_Get_User_Boot_Config(void);
void HBN_Set_User_Boot_Config(uint8_t cfg);

#ifdef __cplusplus
}
#endif
