#define _GNU_SOURCE
#include <string.h>

#include "utils.h"
#include "cores.h"
#include "saves.h"
#include "overlay.h"

// Per-game cartridge quirks, in core_config's low bits (mdtang_top.sv wires
// them to system.sv's quirk inputs; apply_core_config preserves the low 16
// bits, and a game reset only toggles loading, so the word set here stays
// in place for the whole session). Bits 3:0 are MiSTer's EEPROM type/wiring
// encoding, the rest its named-title flags.
#define MDQ_EEPROM_MASK 0x0Fu
#define MDQ_SRAM        (1u << 4)
#define MDQ_SRAM00      (1u << 5)
#define MDQ_NORAM       (1u << 6)
#define MDQ_FMBUSY      (1u << 7)
#define MDQ_SCHAN       (1u << 8)
#define MDQ_ALL         0x1FFu

// The header serial at $183 (8 chars, space padded -- the window MiSTer's
// cartridge.sv matches as cart_id[63:0] while the ROM downloads) picks the
// quirk word. EEPROM contents then live in the normal save path, sized per
// chip (24C01 128 B .. 24C65 8 KB) and rounded up to whole blocks, with the
// chip's bytes first in the .sav exactly as MiSTer lays them out. Titles
// whose hardware the core does not have stay out: Pier Solar (T-574013/
// T-574023, SPI EEPROM plumbing) and Virtua Racing (MK-1229 / G-7001, SVP).
static const struct { const char *serial; uint16_t quirk; } md_quirks[] = {
    // X24C01, SDA/SCL on data bits 7/6 (EEPROM type 1)
    {"T-50446 ", 0x1},  // John Madden Football 93
    {"T-50516 ", 0x1},  // John Madden Football 93 Championship Edition
    {"T-50396 ", 0x1},  // NHLPA Hockey 93
    {"T-50176 ", 0x1},  // Rings of Power
    {"T-50606 ", 0x1},  // Bill Walsh College Football
    // X24C01, SDA read on bit 0 (type 2)
    {"MK-1215 ", 0x2},  // Evander Holyfield's Boxing
    {"G-4060  ", 0x2},  // Wonder Boy in Monster World
    {"00001211", 0x2},  // Sports Talk Baseball
    {"MK-1228 ", 0x2},  // Greatest Heavyweights
    {"G-5538  ", 0x2},  // Greatest Heavyweights (JP)
    {"PR-1993 ", 0x2},  // Greatest Heavyweights (prototype)
    {"00004076", 0x2},  // Honoo no Toukyuuji: Dodge Danpei
    {"T-12046 ", 0x2},  // Mega Man: The Wily Wars
    {"T-12053 ", 0x2},  // Rockman Mega World
    {"G-4524  ", 0x2},  // Ninja Burai Densetsu
    {"00054503", 0x2},  // Game Toshokan
    // 24C02 and up, Acclaim wiring (types 3..7 size the chip)
    {"T-81033 ", 0x3},  // NBA Jam (J)
    {"T-081326", 0x3},  // NBA Jam (U)(E)
    {"T-081276", 0xB},  // NFL Quarterback Club (24C02, paged decode)
    {"T-81406 ", 0xF},  // NBA Jam TE (24C04)
    {"T-081586", 0xC},  // NFL Quarterback Club '96 (24C16)
    {"T-81576 ", 0xD},  // College Slam (24C65)
    {"T-81476 ", 0xD},  // Frank Thomas Big Hurt Baseball (24C65)
    // Codemasters wiring (write $300000, read $380001)
    {"T-120106", 0x6},  // Brian Lara Cricket (24C08)
    {"T-120096", 0x4},  // Micro Machines 2: Turbo Tournament (24C16)
    {"T-120146", 0x5},  // Brian Lara Cricket 96 / Shane Warne Cricket (24C65)
    // SRAM-header quirks for named titles (no EEPROM)
    {"T-113016", MDQ_NORAM},                    // Puggsy (fake RAM check)
    {"T-35036 ", MDQ_FMBUSY},                   // Hellfire (US)
    {"T-25073 ", MDQ_FMBUSY},                   // Hellfire (JP)
    {"MK-1137-", MDQ_FMBUSY},                   // Hellfire (EU)
    {"G-4034  ", MDQ_FMBUSY},                   // Daisenpu / Twin Hawk
    {"T-44016 ", MDQ_FMBUSY},                   // Tecmo World Cup
    {"T-44023 ", MDQ_FMBUSY},                   // Tecmo World Cup (JP)
    {"T-68???-", MDQ_SCHAN},                    // Game no Kanzume Otokuyou
    {" GM 0000", MDQ_SRAM00},                   // Sonic 1 Remastered (blank SRAM is 0x00)
};

// EEPROM size in bytes by type (quirk bits 2:0), MiSTer's mask table + 1.
static const uint16_t md_eeprom_bytes[8] = {0, 128, 128, 256, 2048, 8192, 1024, 512};

// Read the quirk word out of the header: the serial at $183, plus the two
// Codemasters J-Carts MiSTer tells apart by the "DNLD" longword at offset 0
// and the header checksum at $18E. 0 = an ordinary cart.
static uint32_t md_quirk_word(FIL *fp, unsigned int file_size) {
    unsigned char hdr[16];
    unsigned int br = 0;
    if (file_size <= 0x190)
        return 0;
    if (f_lseek(fp, 0x180))
        return 0;
    if (f_read(fp, hdr, 16, &br) != FR_OK || br != 16)
        return 0;
    for (unsigned i = 0; i < sizeof md_quirks / sizeof md_quirks[0]; i++)
        if (memcmp(hdr + 3, md_quirks[i].serial, 8) == 0)
            return md_quirks[i].quirk;
    unsigned char head4[4];
    unsigned int cksum = ((unsigned int)hdr[14] << 8) | hdr[15];
    if (f_lseek(fp, 0) == FR_OK && f_read(fp, head4, 4, &br) == FR_OK && br == 4 &&
        memcmp(head4, "DNLD", 4) == 0) {
        if (cksum == 0x168B) return 0x6;    // Micro Machines Military (24C08)
        if (cksum == 0x165E) return 0x4;    // Micro Machines Turbo Tournament 96 (24C16)
    }
    return 0;
}

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

    // The core's SDRAM ROM window is 8 MB (the largest carts -- Super Street
    // Fighter II at 5 MB, Pier Solar at 8 MB -- are plain binary images and
    // stream like any other size; past 8 MB the loader would wrap into the
    // 68K RAM window, so refuse those instead).
    if (size > 8u * 1024 * 1024) {
        overlay_message("ROM over 8 MB not supported", 1);
        f_close(&fcore);
        return FR_INVALID_OBJECT;
    }

    // Per-game cart quirks from the header serial, and the save size: an
    // EEPROM game's chip size (its header has no "RA" range to read), else
    // the SRAM range as before.
    uint32_t quirks = md_quirk_word(&fcore, size);
    int save_size = (quirks & MDQ_EEPROM_MASK)
                  ? md_eeprom_bytes[quirks & 7]
                  : md_sram_size(&fcore, size);
    saves_set_battery(save_size != 0);
    if (quirks & MDQ_SRAM00)
        saves_set_blank(0x00);
    DEBUG("loadmd: quirks 0x%x, save %d bytes\n", quirks, save_size);

    // The quirk word goes in before the ROM stream, so it is in place when
    // the load reset starts the system (the .gg loader's pattern).
    set_core_config((get_core_config() & ~MDQ_ALL) | quirks);

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
    // loading state. Size rounded up to whole 512-byte blocks and capped at
    // the core's 128 blocks (64 KB); with no battery the engine stays
    // passive. restore() sends the .sav, a recovered .sav.tmp, or the blank
    // image, so the previous game's save cannot leak into this one.
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
