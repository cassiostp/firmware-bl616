#define _GNU_SOURCE
#include <string.h>

#include "utils.h"
#include "cores.h"
#include "overlay.h"
#include "saves.h"

// Load a NES ROM
// return 0 if successful
int loadnes(const char *fname) {
    unsigned int off = 0, br, total = 0;
    unsigned int size;
    int r = 1;
    DEBUG("loadnes start\n");

    // check extension .nes
    if (!has_any_ext(fname, ".nes")) {
        overlay_message("Only .nes supported", 1);
        goto loadnes_end;
    }

    r = f_open(&fcore, fname, FA_READ);
    if (r) {
        overlay_status("Cannot open file");
        goto loadnes_end;
    }
    size = get_file_size(fname);

    // iNES header byte 6 bit 1: battery-backed WRAM. nestang reports every CPU
    // write to the $6000-$7FFF window as a save-RAM write (the core cannot
    // parse the header), so the firmware decides from the header itself:
    // without the bit the WRAM is volatile scratch and no .sav file is made.
    // The streaming below re-seeks to 0, so peeking here is free.
    {
        uint8_t b6 = 0;
        UINT br6 = 0;
        if (f_lseek(&fcore, 6) == FR_OK && f_read(&fcore, &b6, 1, &br6) == FR_OK && br6 == 1)
            saves_set_battery(b6 & 2);
    }

    // load actual ROM
    set_loading_state(1);
    core_running = false;

    // Send rom content
    if ((r = f_lseek(&fcore, off)) != FR_OK) {
        overlay_status("Seek failure");
        goto loadnes_snes_end;
    }


    do {
        if ((r = f_read(&fcore, fbuf, 1024 /*BLOCK_SIZE*/, &br)) != FR_OK)
            break;
        // start rom loading command
        send_fbuf_data(br);
        taskYIELD();                // allow gamepad polling to run
        total += br;
        if ((total & 0xfff) == 0) {	// display progress every 4KB
            //              01234567890123456789012345678901
            overlay_status("%d/%dK                          ", total >> 10, size >> 10);
        }
    } while (br == 1024 /*BLOCK_SIZE*/);

    DEBUG("loadnes: %d bytes\n", total);
    overlay_status("Success");
    // The game's save RAM goes in now, while the core is still held in the
    // loading state: restore() sends the .sav, a recovered .sav.tmp, or the
    // blank image, so the previous game's RAM cannot leak into this one.
    saves_restore();
    core_running = true;

    overlay(0);		// turn off OSD

loadnes_snes_end:
    set_loading_state(0);   // turn off game loading, this starts the core
    f_close(&fcore);
loadnes_end:
    return r;
}
