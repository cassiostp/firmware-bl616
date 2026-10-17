// Host sim shim for Bouffalo bl616_hbn.h.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint8_t HBN_Get_User_Boot_Config(void);
void HBN_Set_User_Boot_Config(uint8_t cfg);
// Set the HBN RTC enable bit without touching the counter (survives MODE).
// The SDK declares this returning BL_Err_Type; int is ABI-compatible here.
int HBN_Enable_RTC_Counter(void);

#ifdef __cplusplus
}
#endif
