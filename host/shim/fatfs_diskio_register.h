// Host sim shim for the FatFs diskio registration helpers.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void fatfs_sdh_driver_register(void);
void fatfs_usbh_driver_register(void);

#ifdef __cplusplus
}
#endif
