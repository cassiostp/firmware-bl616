#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>

#include "utils.h"
#include "cores.h"
#include "saves.h"
#include "overlay.h"
// return 0 if snes header is successfully parsed at off
// typ 0: LoROM, 1: HiROM, 2: ExHiROM
int parse_snes_header(FIL *fp, int pos, int file_size, int typ, unsigned char *hdr,
                      int *map_ctrl, int *rom_type_header, int *rom_size,
                      int *ram_size, int *company) {
    unsigned int br;
    if (f_lseek(fp, pos))
        return 1;
    f_read(fp, hdr, 64, &br);
    if (br != 64) return 1;
    int mc = hdr[21];
    int rom = hdr[23];
    int ram = hdr[24];
    int checksum = (hdr[28] << 8) + hdr[29];
    int checksum_compliment = (hdr[30] << 8) + hdr[31];
    int reset = (hdr[61] << 8) + hdr[60];
    int size2 = 1024 << rom;

    // calc heuristics score
    int score = 0;		
    if (size2 >= file_size) score++;
    if (rom == 1) score++;
    if (checksum + checksum_compliment == 0xffff) score++;
    int all_ascii = 1;
    for (int i = 0; i < 21; i++)
        if (hdr[i] < 32 || hdr[i] > 127)
            all_ascii = 0;
    score += all_ascii;

    // SA-1 BW-RAM reaches 256 KB (ram byte 8); every other cartridge keeps
    // the old cap, so the save code never sees a size it cannot handle yet.
    // An SA-1 ROM is refused below, before any save block count is set.
    int ram_ok = ram <= 7 || (ram == 8 && mc == 0x23 &&
                (hdr[22] == 0x32 || hdr[22] == 0x34 || hdr[22] == 0x35));

    if (rom < 14 && ram_ok && score >= 1 &&
        reset >= 0x8000 &&				// reset vector position correct
       ((typ == 0 && (mc & 3) == 0) || 	// normal LoROM
        (typ == 0 && mc == 0x53)    ||	// contra 3 has 0x53 and LoROM
        (typ == 0 && mc == 0x23)    ||	// SA-1
        (typ == 1 && (mc & 3) == 1) ||	// HiROM
        (typ == 1 && mc == 0x32)    ||	// S-DD1
        (typ == 1 && mc == 0x3a)    ||	// SPC7110
        (typ == 2 && (mc & 3) == 2))) {	// ExHiROM
        *map_ctrl = mc;
        *rom_type_header = hdr[22];
        *rom_size = rom;
        *ram_size = ram;
        *company = hdr[26];
        return 0;
    }
    return 1;
}

// Cartridge chips snestang does not implement yet, keyed on the same
// (map_ctrl, chipset) pairs as the FPGA's src/smc_parser.v, so the loader
// can say what is missing instead of streaming a ROM at a coprocessor that
// is not there (the core falls back to the plain mapper and the game
// hangs). Delete a row when the FPGA gains the chip. Entries smc_parser.v
// cannot tell apart are noted inline. Super FX (GSU: map 0x20, chipset
// 0x13/0x14/0x15/0x1a) is NOT listed on purpose: the GSU core is about to
// be hardware-tested, so GSU ROMs must still load.
static const struct { int map_ctrl; int chipset; const char *name; }
unsupported_chips[] = {
    {0x32, 0x43, "S-DD1"}, {0x32, 0x45, "S-DD1"},
    {0x3a, 0xf5, "SPC7110"}, {0x3a, 0xf9, "SPC7110"},
    {0x30, 0xf5, "ST018"},	// no smc_parser.v case (ST01x is 0xf6): plain-LoROM fallback
};

// BS-X (Satellaview) has no (map_ctrl, chipset) pair and no detection in
// smc_parser.v at all: its cartridges reuse the same 64 bytes with a
// different layout (byte 26 is 0x33 or 0xff, byte 21 is 0 or has bit 7
// set with the low two bits clear, byte 24 holds a normal-bank value and
// byte 22 is 0xff alongside byte 23 or a small multiple of 16; zero in
// both 22 and 23 would be an empty flash pack, not a game). The BS-X
// BIOS itself has a plain header, recognised by its title. A header that
// passed parse_snes_header cannot match the flash-pack test, because its
// byte 24 is a RAM size of at most 8.
static int is_bsx_header(const unsigned char *hdr) {
    if (memcmp(hdr, "Satellaview BS-X     ", 21) == 0)
        return 1;
    if ((hdr[26] == 0x33 || hdr[26] == 0xff) &&
        (hdr[21] == 0 || (hdr[21] & 0x83) == 0x80) &&
        (hdr[24] == 0x20 || hdr[24] == 0x21 || hdr[24] == 0x30 || hdr[24] == 0x31)) {
        int m = hdr[22];
        if ((m == 0xff && hdr[23] == 0xff) ||
            ((m & 0x0f) == 0 && (m >> 4) <= 12 && !(m == 0 && hdr[23] == 0)))
            return 1;
    }
    return 0;
}

// the table above, plus BS-X: NULL if the core can run this cartridge
static const char *unsupported_snes_chip(int map_ctrl, int rom_type_header,
                                         const unsigned char *hdr) {
    for (unsigned i = 0; i < sizeof unsupported_chips / sizeof unsupported_chips[0]; i++)
        if (unsupported_chips[i].map_ctrl == map_ctrl &&
            unsupported_chips[i].chipset == rom_type_header)
            return unsupported_chips[i].name;
    if (is_bsx_header(hdr))
        return "BS-X";
    return NULL;
}

// read the 64 bytes at pos and test them as a BS-X header
static int bsx_header_at(FIL *fp, int pos) {
    unsigned char hdr[64];
    unsigned int br;
    if (f_lseek(fp, pos))
        return 0;
    f_read(fp, hdr, 64, &br);
    return br == 64 && is_bsx_header(hdr);
}

// return 0 if successful
int loadsnes(const char *fname) {
    int r = 1;
    DEBUG("loadsnes start");

    // check extension .sfc or .smc
    if (!has_any_ext(fname, ".smc;.sfc")) {
        overlay_message("Only .smc or .sfc supported", 1);
        return r;
    }

    r = f_open(&fcore, fname, FA_READ);
    if (r) {
        overlay_status("Cannot open file");
        return r;
    }
    unsigned int br, total = 0;
    int size = get_file_size(fname);
    int map_ctrl, rom_type_header, rom_size, ram_size, company;
    // parse SNES header from ROM file
    int off = size & 0x3ff;		// rom header (0 or 512)
    int header_pos;
    overlay_status("snes rom header offset: %d\n", off);
    
    header_pos = 0x7fc0 + off;
    if (parse_snes_header(&fcore, header_pos, size-off, 0, fbuf, &map_ctrl, &rom_type_header, &rom_size, &ram_size, &company)) {
        header_pos = 0xffc0 + off;
        if (parse_snes_header(&fcore, header_pos, size-off, 1, fbuf, &map_ctrl, &rom_type_header, &rom_size, &ram_size, &company)) {
            header_pos = 0x40ffc0 + off;
            if (parse_snes_header(&fcore, header_pos, size-off, 2, fbuf, &map_ctrl, &rom_type_header, &rom_size, &ram_size, &company)) {
                // No ordinary header at any offset. A BS-X flash pack
                // fails all three probes by design; name it rather than
                // claim the file is not a SNES ROM at all.
                if (bsx_header_at(&fcore, 0x7fc0 + off) ||
                    bsx_header_at(&fcore, 0xffc0 + off))
                    overlay_message("BS-X cartridge\nnot supported yet", 1);
                else
                    overlay_message("Not a SNES ROM file", 1);
                r = 1;	// f_open left FR_OK in r: main.cpp must see this
                        // load as failed, not record the ROM as running
                goto loadsnes_close_file;
            }
        }
    }

    // The header parses, but snestang may not have the cartridge's chip:
    // say so and do not load, instead of hanging on a black screen.
    {
        const char *chip = unsupported_snes_chip(map_ctrl, rom_type_header, fbuf);
        if (chip) {
            char msg[48];
            snprintf(msg, sizeof msg, "%s cartridge\nnot supported yet", chip);
            overlay_message(msg, 1);
            r = 1;	// as above: this load did not happen
            goto loadsnes_close_file;
        }
    }

    // load actual ROM
    set_loading_state(1);		// enable game loading, this resets SNES
    core_running = false;

    // Send 64-byte header to snes
    send_fbuf_data(64);

    // Send rom content to snes
    if ((r = f_lseek(&fcore, off)) != FR_OK) {
        overlay_status("Seek failure");
        goto loadsnes_snes_end;
    }
    do {
        if ((r = f_read(&fcore, fbuf, 1024, &br)) != FR_OK)
            break;
        if (br == 0) break;
        send_fbuf_data(br);
        total += br;
        if ((total & 0xffff) == 0) {	// display progress every 64KB
            overlay_status("%d/%dK", total >> 10, size >> 10);
            if ((map_ctrl & 3) == 0)
                overlay_printf(" Lo");
            else if ((map_ctrl & 3) == 1)
                overlay_printf(" Hi");
            else if ((map_ctrl & 3) == 2)
                overlay_printf(" ExHi");
            //              01234567890123456789012345678901
            overlay_printf(" ROM=%d RAM=%d                 ", 1 << rom_size, ram_size ? (1 << ram_size) : 0);
        }
    } while (br == 1024);

    overlay_status("Success");
    // The game's save RAM goes in now, while the core is still held in the
    // loading state. The size comes from the header's SRAM size byte (2^n KB,
    // 0 = this game has no battery RAM): like MiSTer's SNES core, which gates
    // its backup on the same byte (bk_ena <= |ram_mask). restore() sends the
    // .sav, a recovered .sav.tmp, or the blank image, so the previous game's
    // BSRAM cannot leak into this one.
    saves_set_blocks(ram_size ? 2 << ram_size : 0);
    saves_restore();
    core_running = true;

    overlay(0);		// turn off OSD

loadsnes_snes_end:
    set_loading_state(0);	// turn off game loading, this starts SNES
loadsnes_close_file:
    f_close(&fcore);
    return r;
}
