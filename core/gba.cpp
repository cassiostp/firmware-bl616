#define _GNU_SOURCE
#include <string.h>

#include "utils.h"
#include "cores.h"
#include "overlay.h"
#include "saves.h"
#include "settings.h"
#include "fastload.h"

// Core specific state
bool gba_bios_loaded;
bool gba_missing_bios_warned;
static uint8_t gba_backup_type;         // config code sent for the loaded game, 0 = none
static uint8_t gba_gpio_flags;          // bit0: cart RTC, bit1: solar sensor (loader byte 1)

// Cartridge GPIO devices are announced by the 4-char game code at ROM offset
// 0xAC (first 3 chars identify the game, the 4th is the region). The tables
// in GBA_MiSTer (GBA.sv gpio_quirk/solar_quirk) and mGBA (overrides.c) agree
// on these; gyro/rumble/tilt carts are deliberately not listed.
static const struct { char code[4]; uint8_t flags; } gba_gpio_ids[] = {
    {"AXV", 1},     // Pokemon Ruby
    {"AXP", 1},     // Pokemon Sapphire
    {"BPE", 1},     // Pokemon Emerald
    {"BKA", 1},     // Sennen Kazoku
    {"BR4", 1},     // Rockman EXE 4.5
    {"BLJ", 1},     // mGBA HW_RTC entries BLJJ/BLJK
    {"BLV", 1},     // mGBA HW_RTC entry BLVJ
    {"U3I", 3},     // Boktai: The Sun is in Your Hand (RTC + solar)
    {"U32", 3},     // Boktai 2: Solar Boy Django
    {"U33", 3},     // Boktai 3 (JP)
};

uint8_t gba_gpio_flags_for(const char *game_code) {
    for (unsigned i = 0; i < sizeof gba_gpio_ids / sizeof gba_gpio_ids[0]; i++)
        if (memcmp(game_code, gba_gpio_ids[i].code, 3) == 0)
            return gba_gpio_ids[i].flags;
    return 0;
}

bool gba_uses_solar(void) {
    return (gba_gpio_flags & 2) != 0;
}

uint8_t gba_current_gpio_flags(void) {
    return gba_gpio_flags;
}

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
// if not, warn user, if present, load it. Streams at the current link rate
// and, on a fast link, checks the BIOS window (loading state 4) the same
// way as the ROM. Returns false only when the BIOS file cannot be read
// (missing BIOS is fine: the core falls back to its open-source BIOS).
static bool gba_load_bios(int *link_code) {
    if (gba_bios_loaded | gba_missing_bios_warned) return true;

    DEBUG("gba_load_bios start\n");
    FILINFO fno;
    std::string bios_path(drv);
    bios_path.append("gba/gba_bios.bin");
    if (f_stat(bios_path.c_str(), &fno) != FR_OK) {
        overlay_message( "Cannot find gba_bios.bin\n"
                 "Using open source BIOS\n"
                 "Expect low compatibility", 1);
        gba_missing_bios_warned = 1;
        return true;
    }

    bool ok = true;
    unsigned br;
    if (f_open(&fcore, bios_path.c_str(), FA_READ) != FR_OK) {
        overlay_message("Cannot open /gba/gba_bios.bin", 1);
        return false;
    }
    set_loading_state(4);
    uint32_t crc = 0xFFFFFFFF, cnt = 0;
    FRESULT r;
    do {
        if ((r = f_read(&fcore, fbuf, BLOCK_SIZE, &br)) != FR_OK) {
            ok = false;
            break;
        }
        for (unsigned o = 0; o < br; o += 1024) {
            unsigned n = br - o > 1024 ? 1024 : br - o;
            send_data_frame(fbuf + o, (uint16_t)n);
            crc = fast_crc32_update(crc, fbuf + o, n);
            cnt += n;
        }
    } while (br == BLOCK_SIZE);

    if (ok && *link_code != FAST_BAUD_2M && !fast_verify(crc ^ 0xFFFFFFFF, cnt)) {
        // Same full-retry shape as the ROM below: back to 2M, whole BIOS again.
        DEBUG("gba_load_bios: fast verify failed, retry at 2M\n");
        *link_code = fast_negotiate(FAST_BAUD_2M, *link_code);
        if (*link_code != FAST_BAUD_2M) {
            delay(1200);                    // the core's watchdog reverts it
            fast_set_baud(2000000);
            *link_code = FAST_BAUD_2M;
        }
        f_lseek(&fcore, 0);
        set_loading_state(4);
        crc = 0xFFFFFFFF; cnt = 0;
        do {
            if ((r = f_read(&fcore, fbuf, BLOCK_SIZE, &br)) != FR_OK) {
                ok = false;
                break;
            }
            for (unsigned o = 0; o < br; o += 1024) {
                unsigned n = br - o > 1024 ? 1024 : br - o;
                send_data_frame(fbuf + o, (uint16_t)n);
                crc = fast_crc32_update(crc, fbuf + o, n);
                cnt += n;
            }
        } while (br == BLOCK_SIZE);
        if (ok && !fast_verify(crc ^ 0xFFFFFFFF, cnt))
            DEBUG("gba_load_bios: 2M verify failed too (proceeding)\n");
    }

    f_close(&fcore);
    gba_bios_loaded = 1;
    DEBUG("gba_load_bios end\n");
    return ok;
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
    uint8_t tail[9];                    // backup ID scan: prev chunk's tail
    uint32_t tlen = 0, bhits = 0;       // (declared before the gotos below)
    uint8_t btype = 0;                  // the detected backup chip
    uint16_t bblocks = 64;              // its save size in blocks (256 does not fit in uint8_t)
    char game_code[4] = {0, 0, 0, 0};   // ROM offset 0xAC, from the first chunk
    memset(tail, 0, sizeof tail);

    // The seek runs before the streaming state below so a failure returns
    // early, the same way loadgba_close at the end does.
    if ((r = f_lseek(&fcore, 0)) != FR_OK) {
        overlay_status("Seek failure");
        set_loading_state(0);   // turn off game loading, this starts the core
        f_close(&fcore);
        return r;
    }

    // Negotiate the fast link first: 8M, else 4M, else 2M exactly as today.
    // A core without FAST_UART stays silent through both probes (120 ms),
    // and every byte after that is the proven 2M path.
    int link_code = fast_negotiate(FAST_BAUD_8M, FAST_BAUD_2M);
    if (link_code != FAST_BAUD_8M)
        link_code = fast_negotiate(FAST_BAUD_4M, FAST_BAUD_2M);
    DEBUG("loadgba: link code %d\n", link_code);

    // load actual ROM: 8 KB SD reads framed as 1 KB 0x07 payloads (the frame
    // length tops out at 2047), with the CRC-32 and byte count the core
    // checks on 0x16.
    set_loading_state(1);		// enable game loading, this resets GBA
    core_running = false;

    uint32_t rom_crc = 0xFFFFFFFF, rom_cnt = 0;
    unsigned br = 0, total = 0;
    for (;;) {
        if ((r = f_read(&fcore, fbuf, BLOCK_SIZE, &br)) != FR_OK)
            break;
        if (br == 0)
            break;
        for (unsigned o = 0; o < br; o += 1024) {
            unsigned n = br - o > 1024 ? 1024 : br - o;
            bhits |= gba_scan_backup(fbuf + o, n, tail, &tlen);
            if (total + o == 0 && n > 0xB0)    // game code at 0xAC is in frame 0
                memcpy(game_code, fbuf + 0xAC, 4);
            send_data_frame(fbuf + o, (uint16_t)n);
            rom_crc = fast_crc32_update(rom_crc, fbuf + o, n);
            rom_cnt += n;
        }

        total += br;
        if ((total & 0xffff) == 0) {	// display progress every 64KB
            //              01234567890123456789012345678901
            overlay_status("%d/%dK                          ", total >> 10, size >> 10);
        }
        if (br < BLOCK_SIZE)
            break;
    }

    DEBUG("loadgba: %d bytes rom sent.\n", total);

    if (r == FR_OK && link_code != FAST_BAUD_2M &&
            !fast_verify(rom_crc ^ 0xFFFFFFFF, rom_cnt)) {
        // The stream the core saw is not the file: the whole ROM goes again
        // at 2M (the core's window reopens at loading==1, so the retry is a
        // complete load, not a patch).
        overlay_status("Load check failed, retrying at 2M...");
        DEBUG("loadgba: fast verify failed, retry at 2M\n");
        link_code = fast_negotiate(FAST_BAUD_2M, link_code);
        if (link_code != FAST_BAUD_2M) {
            delay(1200);                    // the core's watchdog reverts it
            fast_set_baud(2000000);
            link_code = FAST_BAUD_2M;
        }
        memset(tail, 0, sizeof tail);
        tlen = 0; bhits = 0;
        memset(game_code, 0, sizeof game_code);
        set_loading_state(1);
        rom_crc = 0xFFFFFFFF; rom_cnt = 0; total = 0;
        r = f_lseek(&fcore, 0);
        if (r == FR_OK) {
            for (;;) {
                if ((r = f_read(&fcore, fbuf, BLOCK_SIZE, &br)) != FR_OK)
                    break;
                if (br == 0)
                    break;
                for (unsigned o = 0; o < br; o += 1024) {
                    unsigned n = br - o > 1024 ? 1024 : br - o;
                    bhits |= gba_scan_backup(fbuf + o, n, tail, &tlen);
                    if (total + o == 0 && n > 0xB0)
                        memcpy(game_code, fbuf + 0xAC, 4);
                    send_data_frame(fbuf + o, (uint16_t)n);
                    rom_crc = fast_crc32_update(rom_crc, fbuf + o, n);
                    rom_cnt += n;
                }
                total += br;
                if ((total & 0xffff) == 0)
                    overlay_status("%d/%dK                          ", total >> 10, size >> 10);
                if (br < BLOCK_SIZE)
                    break;
            }
        }
        DEBUG("loadgba: retry sent %d bytes.\n", total);
        if (r == FR_OK && !fast_verify(rom_crc ^ 0xFFFFFFFF, rom_cnt))
            DEBUG("loadgba: 2M verify failed too (proceeding)\n");
    }

    f_close(&fcore);          // the ROM is fully streamed; the BIOS opens fresh

    gba_load_bios(&link_code);

    // Back to 2M for the small stuff and the save traffic: the proven rate,
    // and a fast link would revert by itself after 1 s idle anyway.
    // saw_fast remembers whether this core speaks the fast protocol: only
    // then does the region-3 check below have an answer to wait for (an old
    // core would just eat two 500 ms timeouts).
    bool saw_fast = link_code != FAST_BAUD_2M;
    if (link_code != FAST_BAUD_2M) {
        link_code = fast_negotiate(FAST_BAUD_2M, link_code);
        if (link_code != FAST_BAUD_2M) {
            delay(1200);
            fast_set_baud(2000000);
            link_code = FAST_BAUD_2M;
        }
    }

    // Tell the core which backup chip to emulate (loader region 3; the core
    // zeroes the setting at loading==1, so it goes in after the ROM stream),
    // then load this game's save file into it. No ID string: no battery; the
    // 32 KB SRAM window still gets the blank image so the previous game's
    // RAM cannot leak into a game that expects to find it uninitialized.
    for (unsigned i = 0; btype == 0 && i < sizeof gba_backup_ids / sizeof gba_backup_ids[0]; i++)
        if (bhits & (1u << i)) { btype = gba_backup_ids[i].type; bblocks = btype == 2 ? 256 : btype == 1 ? 128 : btype == 4 ? 16 : 64; }
    gba_backup_type = btype;
    gba_gpio_flags = gba_gpio_flags_for(game_code);
    // Loader region 3 config, in order: backup type, GPIO flags, solar %.
    // Always send all three: the core zeroes them at loading==1, and an old
    // bitstream simply ignores the extra bytes. Sent at 2M and verified: it
    // is only 3 bytes, and the check also proves the switchback above
    // really put both ends back on the proven rate.
    fbuf[0] = btype;
    fbuf[1] = gba_gpio_flags;
    fbuf[2] = settings.solar_pct;
    set_loading_state(3);
    send_fbuf_data(3);
    if (saw_fast) {
        uint32_t cfg_crc = fast_crc32_update(0xFFFFFFFF, fbuf, 3);
        if (!fast_verify(cfg_crc ^ 0xFFFFFFFF, 3)) {
            DEBUG("loadgba: region-3 verify failed, resending\n");
            set_loading_state(3);
            send_fbuf_data(3);
            if (!fast_verify(cfg_crc ^ 0xFFFFFFFF, 3))
                DEBUG("loadgba: region-3 reverify failed too (proceeding)\n");
        }
    }
    if (!btype)
        saves_set_battery(false);
    saves_set_blocks(bblocks);      // 64 (blank wipe only) if there is no battery
    // RTC games keep their clock in one extra .sav sector (the core's
    // MiSTer-format "RT" block), restored and dumped with the save RAM.
    saves_set_extra_blocks((gba_gpio_flags & 1) ? 1 : 0);
    saves_restore();
    DEBUG("loadgba: backup type %d, %d blocks, gpio %d (code %.4s)\n",
          btype, bblocks, gba_gpio_flags, game_code);

    overlay_status("Success");
    core_running = true;

    overlay(0);		// turn off OSD

    set_loading_state(0);   // turn off game loading, this starts the core
    f_close(&fcore);
    return r;
}

// gba_memory clears the loader config whenever loading goes to 1, which a
// game reset does too. Call between set_loading_state(1) and
// set_loading_state(0) of a reset so the restarted game still finds its
// save chip and its GPIO devices.
void gba_resend_backup_type(void) {
    fbuf[0] = gba_backup_type;
    fbuf[1] = gba_gpio_flags;
    fbuf[2] = settings.solar_pct;
    set_loading_state(3);
    send_fbuf_data(3);
}
