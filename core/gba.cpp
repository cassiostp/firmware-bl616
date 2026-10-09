#define _GNU_SOURCE
#include <string.h>

#include "utils.h"
#include "cores.h"
#include "overlay.h"
#include "saves.h"

// Core specific state
bool gba_bios_loaded;
bool gba_missing_bios_warned;
static uint8_t gba_backup_type;         // config code sent for the loaded game, 0 = none

// Cart backup chips announce themselves in the ROM with an ID string. These
// are the ones GBA games ship; the byte is the core's config code (gba_memory
// loader region 3: 1 FLASH512K, 2 FLASH1M, 3 SRAM, 4 EEPROM) and the pair is
// what GBA_MiSTer's .sav sizes agree with -- its <rom>.sav is the raw backup
// RAM, 16/64/128/256 512-byte blocks for EEPROM/SRAM/FLASH/FLASH1M (save_sz in
// GBA.sv, which itself scans the stream for "FLASH1M_V"). Flash beats SRAM
// beats EEPROM: library stubs mentioning a lesser chip must not mask the real
// one. "SRAM_F_V" is FRAM, wired exactly like SRAM.
static const struct { const char *id; uint8_t type; } gba_backup_ids[] = {
    {"FLASH1M_V",  2},
    {"FLASH512_V", 1},
    {"FLASH_V",    1},
    {"SRAM_F_V",   3},
    {"SRAM_V",     3},
    {"EEPROM_V",   4},
};

// Scan one ROM chunk for the ID strings. The strings can straddle the 1024-
// byte chunk boundaries, so the scan window carries the previous chunk's last
// 9 bytes (longest ID minus one) in front of it. Returns a bitmask over
// gba_backup_ids; the callers resolve overlaps in table order.
static uint32_t gba_scan_backup(const uint8_t *p, uint32_t n, uint8_t *tail, uint32_t *tn) {
    uint8_t win[9 + 1024];
    memcpy(win, tail, *tn);
    memcpy(win + *tn, p, n);
    uint32_t hits = 0;
    for (unsigned i = 0; i < sizeof gba_backup_ids / sizeof gba_backup_ids[0]; i++)
        if (memmem(win, *tn + n, gba_backup_ids[i].id, strlen(gba_backup_ids[i].id)))
            hits |= 1u << i;
    uint32_t nt = *tn + n < 9 ? *tn + n : 9;            // new tail: window's end
    memcpy(tail, win + *tn + n - nt, nt);
    *tn = nt;
    return hits;
}


// check if gba_bios.bin is present in the root directory
// if not, warn user, if present, load it
void gba_load_bios() {
    if (gba_bios_loaded | gba_missing_bios_warned) return;

    DEBUG("gba_load_bios start\n");
    FILINFO fno;
    std::string bios_path(drv);
    bios_path.append("gba/gba_bios.bin");
    if (f_stat(bios_path.c_str(), &fno) != FR_OK) {
        overlay_message( "Cannot find gba_bios.bin\n"
                 "Using open source BIOS\n"
                 "Expect low compatibility", 1);
        gba_missing_bios_warned = 1;
        return;
    }

    int r = 1;
    unsigned br;
    if (f_open(&fcore, bios_path.c_str(), FA_READ) != FR_OK) {
        overlay_message("Cannot open /gba/gba_bios.bin", 1);
        return;
    }
    set_loading_state(4);
    do {
        if ((r = f_read(&fcore, fbuf, 1024, &br)) != FR_OK)
            break;
        send_fbuf_data(br);
    } while (br == 1024);

    f_close(&fcore);
    gba_bios_loaded = 1;
    DEBUG("gba_load_bios end\n");
}

int loadgba(const char *fname) {
    DEBUG("loadgba start\n");
    FRESULT r = FR_NO_FILE;

    // check extension .gba (suffix match: gba_bios.bin is not a ROM)
    if (!has_any_ext(fname, ".gba")) {
        overlay_message("Only .gba supported", 1);
        return r;
    }

    unsigned int size = get_file_size(fname);

    r = f_open(&fcore, fname, FA_READ);
    if (r) {
        overlay_status("Cannot open file");
        return r;
    }
    unsigned int off = 0, br, total = 0;
    uint8_t tail[9];                    // backup ID scan: prev chunk's tail
    uint32_t tlen = 0, bhits = 0;       // (declared before the gotos below)
    uint8_t btype = 0, bblocks = 64;    // the detected backup chip
    memset(tail, 0, sizeof tail);

    // load actual ROM
    set_loading_state(1);		// enable game loading, this resets GBA
    core_running = false;

    // Send rom content to gba
    if ((r = f_lseek(&fcore, off)) != FR_OK) {
        overlay_status("Seek failure");
        goto loadgba_close;
    }
    do {
        if ((r = f_read(&fcore, fbuf, 1024, &br)) != FR_OK)
            break;

        bhits |= gba_scan_backup(fbuf, br, tail, &tlen);
        send_fbuf_data(br);

        total += br;
        if ((total & 0xffff) == 0) {	// display progress every 64KB
            //              01234567890123456789012345678901
            overlay_status("%d/%dK                          ", total >> 10, size >> 10);
        }
    } while (br == 1024);

    DEBUG("loadgba: %d bytes rom sent.\n", total); 

    gba_load_bios();

    // Tell the core which backup chip to emulate (loader region 3; the core
    // zeroes the setting at loading==1, so it goes in after the ROM stream),
    // then load this game's save file into it. No ID string: no battery; the
    // 32 KB SRAM window still gets the blank image so the previous game's
    // RAM cannot leak into a game that expects to find it uninitialized.
    for (unsigned i = 0; btype == 0 && i < sizeof gba_backup_ids / sizeof gba_backup_ids[0]; i++)
        if (bhits & (1u << i)) { btype = gba_backup_ids[i].type; bblocks = btype == 2 ? 256 : btype == 1 ? 128 : btype == 4 ? 16 : 64; }
    gba_backup_type = btype;
    if (btype) {
        fbuf[0] = btype;
        set_loading_state(3);
        send_fbuf_data(1);
    } else
        saves_set_battery(false);
    saves_set_blocks(bblocks);      // 64 (blank wipe only) if there is no battery
    saves_restore();
    DEBUG("loadgba: backup type %d, %d blocks\n", btype, bblocks);

    overlay_status("Success");
    core_running = true;

    overlay(0);		// turn off OSD

loadgba_close:
    set_loading_state(0);   // turn off game loading, this starts the core
    f_close(&fcore);
    return r;
}

// gba_memory clears the backup type whenever loading goes to 1, which a game
// reset does too. Call between set_loading_state(1) and set_loading_state(0)
// of a reset so the restarted game still finds its save chip.
void gba_resend_backup_type(void) {
    if (!gba_backup_type) return;
    fbuf[0] = gba_backup_type;
    set_loading_state(3);
    send_fbuf_data(1);
}
