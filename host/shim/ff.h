// Host sim "FatFs": implements the FatFs API subset the firmware uses,
// backed directly by a host directory (the virtual SD card). Drive prefixes
// ("sd:", "usb:") both map to that directory; see sim/ffsim.cpp.
//
// This is NOT ChaN's FatFs: there is no FAT image, no diskio layer. File
// semantics from the firmware's point of view (open/read/write/seek/close,
// stat, mkdir, unlink, rename, alphabetical directory listing) match.
#pragma once

#include <stdio.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef unsigned int UINT;

typedef enum {
    FR_OK = 0,
    FR_DISK_ERR,
    FR_INT_ERR,
    FR_NOT_READY,
    FR_NO_FILE,
    FR_NO_PATH,
    FR_INVALID_NAME,
    FR_DENIED,
    FR_EXIST,
    FR_INVALID_OBJECT,
    FR_WRITE_PROTECTED,
    FR_INVALID_DRIVE,
    FR_NOT_ENABLED,
    FR_NO_FILESYSTEM,
    FR_MKFS_ABORTED,
    FR_TIMEOUT,
    FR_LOCKED,
    FR_NOT_ENOUGH_CORE,
    FR_TOO_MANY_OPEN_FILES,
    FR_INVALID_PARAMETER
} FRESULT;

#define FA_READ 0x01
#define FA_WRITE 0x02
#define FA_OPEN_EXISTING 0x00
#define FA_CREATE_NEW 0x04
#define FA_CREATE_ALWAYS 0x08
#define FA_OPEN_ALWAYS 0x10
#define FA_OPEN_APPEND 0x30

#define AM_RDO 0x01
#define AM_HID 0x02
#define AM_SYS 0x04
#define AM_DIR 0x10
#define AM_ARC 0x20

typedef struct {
    int _sim_dummy;
} FATFS;

typedef struct {
    FILE *_sim_fp;
} FIL;

typedef struct {
    void *_sim_opaque;
} DIR;

typedef struct {
    char fname[256];
    DWORD fsize;
    WORD fdate;
    WORD ftime;
    BYTE fattrib;
} FILINFO;

FRESULT f_mount(FATFS *fs, const char *path, BYTE opt);
FRESULT f_open(FIL *fp, const char *path, BYTE mode);
FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br);
FRESULT f_write(FIL *fp, const void *buff, UINT btw, UINT *bw);
FRESULT f_lseek(FIL *fp, DWORD ofs);
FRESULT f_close(FIL *fp);
FRESULT f_sync(FIL *fp);
FRESULT f_stat(const char *path, FILINFO *fno);
FRESULT f_opendir(DIR *dp, const char *path);
FRESULT f_readdir(DIR *dp, FILINFO *fno);
FRESULT f_closedir(DIR *dp);
FRESULT f_mkdir(const char *path);
FRESULT f_unlink(const char *path);
FRESULT f_rename(const char *old_path, const char *new_path);

// Sim-only: point the "sd:"/"usb:" drives at a host directory.
void ffsim_set_root(const char *dir);

#ifdef __cplusplus
}
#endif
