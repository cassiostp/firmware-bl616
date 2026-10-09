#define _GNU_SOURCE
#include <string.h>

#include "utils.h"
#include "cores.h"
#include "saves.h"
#include "overlay.h"

// The Sega spec puts the backup-RAM info in the ROM header at $1B0: the
// device name "RA" (battery-backed RAM installed), two device/type bytes,
// then the big-endian SRAM start/end addresses at $1B4/$1B8 (MiSTer's
// MegaDrive core loads <rom>.sav for every game, so it reads none of this;
// we use it to size the save and to skip games that have no battery).
// The address range only sizes the image here: mdtang decodes cart SRAM at the
// $200000-$37FFFF window and uses only A[16:1] (system.sv), so every window
// and mirror aliases onto image bytes 0.., and .bin/.md/.gen are plain
// binary -- the header sits at file offset $1B0 with no SMD interleave.
// Returns the save size in bytes, 0 if this ROM has no backup RAM.
static int md_sram_size(FIL *fp, unsigned int file_size) {
    unsigned char hdr[12];
    unsigned int br = 0;
    if (file_size <= 0x1bc)
        return 0;                       // too small to carry a header
    if (f_lseek(fp, 0x1b0))
        return 0;
    if (f_read(fp, hdr, 12, &br) != FR_OK || br != 12)
        return 0;
    if (!((hdr[0] == 'R' && hdr[1] == 'A') || (hdr[0] == 'r' && hdr[1] == 'a')))
        return 0;                       // no backup RAM
    unsigned int start = ((unsigned int)hdr[4] << 24) | (hdr[5] << 16) | (hdr[6] << 8) | hdr[7];
    unsigned int end = ((unsigned int)hdr[8] << 24) | (hdr[9] << 16) | (hdr[10] << 8) | hdr[11];
    if (end < start)
        return 0;                       // malformed range: nothing to save
    // The image is laid out from $200000 (image byte N = cart byte $200000+N),
    // so it must reach `end`, not just span end-start: SRAM on odd bytes only
    // starts at $200001, e.g. Phantasy Star II's $200001-$203FFF is 16 KB of
    // image. Ranges outside the window alias in the core; keep the whole 64 KB.
    if (start < 0x200000 || end >= 0x210000)
        return 0x10000;
    return end - 0x200000 + 1;
}

int loadmd(const char *fname) {
    DEBUG("loadmd start\n");
    FRESULT r = FR_NO_FILE;

    // check extension .bin/.md/.gen (.smd is interleaved and would need converting)
    if (!has_any_ext(fname, ".bin;.md;.gen")) {
        overlay_message("Only .bin/.md/.gen supported", 1);
        return r;
    }

    r = f_open(&fcore, fname, FA_READ);
    if (r) {
        overlay_status("Cannot open file");
        return r;
    }
    unsigned int off = 0, br, total = 0;
    unsigned int size = get_file_size(fname);

    // the game's save RAM size, from the header, before the restore below
    int save_size = md_sram_size(&fcore, size);
    saves_set_battery(save_size != 0);

    // load actual ROM
    set_loading_state(1);		// enable game loading, this resets the core
    core_running = false;

    // Send rom content to core
    if ((r = f_lseek(&fcore, off)) != FR_OK) {
        overlay_status("Seek failure");
        goto loadmd_close_file;
    }
    do {
        if ((r = f_read(&fcore, fbuf, 1024, &br)) != FR_OK)
            break;
        send_fbuf_data(br);
        taskYIELD();                // allow gamepad polling to run
        total += br;
        if ((total & 0xfff) == 0) {	// display progress every 4KB
            //              01234567890123456789012345678901
            overlay_status("%d/%dK                          ", total >> 10, size >> 10);
        }
    } while (br == 1024);

    DEBUG("loadmd: %d bytes\n", total);
    overlay_status("Success");
    // The game's save RAM goes in now, while the core is still held in the
    // loading state. Size from the header, rounded up to whole 512-byte
    // blocks and capped at the core's 128 blocks (64 KB); with no "RA" the
    // engine stays passive. restore() sends the .sav, a recovered .sav.tmp,
    // or the blank image, so the previous game's SRAM cannot leak into this
    // one.
    if (save_size)
        saves_set_blocks((save_size + 511) >> 9);
    saves_restore();
    core_running = true;

    overlay(0);		// turn off OSD

loadmd_close_file:
    set_loading_state(0);   // turn off game loading, this starts the core
    f_close(&fcore);
    return r;
}
