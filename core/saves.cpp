// Battery saves: each core's save RAM survives on the SD card.
//
// The FPGA exposes the core's save RAM through the iosys save channel (the same
// protocol as rtissera's firmware, core/pcesave.cpp, Apache-2.0):
//   MCU -> FPGA 0x11 blk[15:0] <512 bytes>   write one block (restore at game load)
//   MCU -> FPGA 0x12 blk[15:0]               request one block
//   FPGA-> MCU 0x0A blk[15:0] <512 bytes>   the requested block
//   FPGA-> MCU 0x0B <pad byte>              the game wrote save RAM since the last dump
// Old bitstreams ignore 0x11/0x12 and never send 0x0B, so with them nothing is
// ever dumped or written back.
//
// WHEN WE SAVE
//   - 1 s after the FPGA's 0x0B notice (4 s if the game was dumped less than
//     10 s ago: a game that scribbles save RAM as work RAM gets a dump every
//     few seconds, not a card write every second). The FPGA sends one 0x0B per
//     clean-to-dirty change, so the delay runs from the first write, and one
//     dump covers a burst of writes.
//   - when a menu opens over the running game, and before a different ROM/core
//     loads or the game closes/resets: saves_settle() flushes synchronously.
//   - never during a game switch: the old game's pending save is flushed first,
//     under the old file's name, then dropped with it. The new game's RAM can
//     never be dumped under the old name.
//   - a dump that fails (a block never arrives, the card refuses the file) is
//     retried every 3 s, up to 5 times.
// MODE is the one way to lose a save: it reloads the FPGA, which drops the
// MCU's UART and restarts the firmware with no chance to flush. A write made
// less than the delay above (1 s + the dump, ~0.1-0.3 s) before MODE is lost.
// saves_status_text() says what happened to the last save, so the game menu
// can show "Saved 12s ago" before anyone presses MODE.
//
// The file is written as <name>.sav.tmp, f_sync'd, then the old .sav is unlinked
// and the .tmp renamed up, so a power cut mid-write leaves the previous save.
// A dump identical to the file's contents writes nothing (no card wear; a save
// in the same game twice does not touch the card the second time).
//
// FATFS USE: FatFs here is not reentrant (FF_FS_REENTRANT=0). Everything this
// task does with FatFs runs under sv_mutex, and the task only dumps while a
// game is armed as running (sv_live). Every path where main_task touches FatFs
// over a running game (menus, file browsers, ROM loading) goes through
// saves_settle()/saves_set_game(), which waits for an in-flight dump and stops
// new ones. So the save task and main_task are never inside FatFs at once.
// (The floppy writes in uart1_rx_task are a pre-existing exception, unchanged.)
//
// SIZE: most cores have one fixed save geometry; the SNES' is per game, from
// the ROM header's SRAM size byte (see core/snes.cpp), handed in with
// saves_set_blocks() before the restore. A 128 KB game (256 blocks) takes
// ~2 ms of UART per block on a ~0.5 s pass; sv_fetch_block() bumps the
// heartbeat per block, so the watchdog never fires mid-dump.
#include "saves.h"

extern "C" {
#include "ff.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "bflb_mtimer.h"
}
#include <string.h>
#include <stdio.h>

#include "utils.h"
#include "uart_rx.h"
#include "overlay.h"
#include "settings.h"

// Per-core save geometry. Blocks are 512 bytes, like the wire protocol.
struct save_geom {
    uint16_t core_id;
    uint16_t blocks;
    const char *dir;        // folder under <drive>saves/
    uint8_t blank;          // byte an empty save RAM comes up as
    bool pause_dump;        // the game shares the save RAM's port: pause it while dumping
};
static const save_geom save_geoms[] = {
    // SMS: the core's 32 KB nvram. MiSTer's SMS core (SMS_MiSTer) keeps the
    // same layout, raw and interchangeable: its backup RAM is a dpram
    // widthad_a=15 (SMS.sv), saved 64 x 512 bytes straight to <rom>.sav, and
    // its empty-RAM init file (rtl/nvram_ff.mif) is all 0xFF, not 0x00.
    {5, 64, "sms", 0xFF, false},     // dual-port on-chip RAM: no pause
    // SNES: the block count is per game, from the ROM header's SRAM size byte
    // (saves_set_blocks; 0 = the game has no battery RAM). Two of MiSTer's
    // SNES core (SNES_MiSTer) details carry over: the backup RAM is dumped
    // raw as 512-byte sectors ({sd_lba, sd_buff_addr} address its dpram
    // directly, SNES.sv), a fresh/blank BSRAM is 0xFF -- the core "thrashes"
    // it with 8'hFF on every ROM load (data_a = clearing_ram ? 8'hFF, SNES.sv)
    // -- and battery saves are gated on the header's RAM-size byte being
    // non-zero (bk_ena <= |ram_mask).
    // The save channel shares the SDRAM BSRAM port with the game, so the game
    // is paused while a dump runs (~2.6 ms per block, twice).
    {2, 0, "snes", 0xFF, true},
    // NES: the 8 KB WRAM window every battery mapper routes through linear
    // 0x3C0000 (see nestang's sdram_nes save channel). MiSTer's NES core
    // (NES_MiSTer) also keeps save RAM as a raw <rom>.sav, in saves/NES/
    // (README, "Saving and Loading"); its cart RAM rides the SDRAM CARTRAM
    // area ({7'b0001111, save_addr[17:0]} in NES.sv), which has no defined
    // power-up value — like our own SDRAM-backed WRAM. A real NES leaves
    // uninitialized RAM to chance and games that ship with a battery must
    // tolerate it, so there is no "right" blank byte: we pick 0xFF, matching
    // the SMS entry and the mostly-high power-up state of SRAM chips.
    {1, 16, "nes", 0xFF, false},  // own SDRAM slot, idle-only: no pause
    // GBA: the block count is per game, from the backup ID string in the ROM
    // (saves_set_blocks in core/gba.cpp: EEPROM 16, SRAM 64, FLASH 128,
    // FLASH1M 256 blocks). MiSTer's GBA core (GBA_MiSTer) keeps the same raw
    // layout and is interchangeable: its <rom>.sav is a plain dump of the
    // backup RAM at those same sizes -- save_sz is 8'hF / 8'h3F /
    // {flash_1m, 7'h7F} sectors in GBA.sv, and it tells 512K from 1M flash by
    // scanning the ROM stream for "FLASH1M_V", the same string we match.
    // gbatang's save channel is an idle-only SDRAM client (and the EEPROM's
    // second port), so a dump never delays the game: no pause.
    {3, 0, "gba", 0xFF, false},
    // MegaDrive / Genesis: the cart SRAM the core decodes at $200000-$37FFFF,
    // streamed byte-wise from its own SDRAM slot (sdram.v port 3, lowest
    // priority: never delays the game, so no pause). MiSTer's MegaDrive core
    // (MegaDrive_MiSTer) keeps the same raw linear image: its save stream is
    // save_addr={sd_lba[6:0],sd_buff_addr} over the SRAM indexed by
    // cart_addr[16:1] (cartridge.sv, MegaDrive.sv), i.e. <rom>.sav = cart
    // bytes $200000.. , 32K words = 128 x 512-byte blocks = 64 KB; and its
    // fresh SRAM is 0xFF (the load-time fill sram2_di = 16'hFFFF, cartridge.sv).
    // The block count is per game (saves_set_blocks): the ROM header's "RA"
    // at $1B0 flags backup RAM, with its start/end addresses at $1B4/$1B8;
    // serial-EEPROM carts instead get their chip size (core/md.cpp), and the
    // core steers those blocks at its EEPROM BRAM, same .sav layout.
    // MiSTer has no saves folder for the MegaDrive (the .sav lives beside the
    // ROM); this is our folder token for core 4, same word as cores.cpp's
    // rom_dir.
    {4, 0, "genesis", 0xFF, false},
};
#define N_GEOMS (sizeof(save_geoms) / sizeof(save_geoms[0]))

#define SV_BLK 512

// FatFs objects and file buffers must not be cached (see fbuf in main.cpp). One
// FIL serves every path: the sv_mutex guarantees no two save file operations run at once.
USB_NOCACHE_RAM_SECTION FIL fsave;
USB_NOCACHE_RAM_SECTION static uint8_t sv_io[SV_BLK];     // file <-> wire staging
USB_NOCACHE_RAM_SECTION static uint8_t sv_rx[SV_BLK];     // the block the RX task is filling
static char sv_path[256];                   // "" = no game with battery saves
static char sv_dir[256];                    // "<drive>saves/<core dir>"
static char sv_root[256];                   // "<drive>saves"
static const save_geom *sv_geom;            // geometry for sv_path
static uint16_t sv_blocks;                  // its block count; per-game for SNES
static uint8_t sv_blank = 0xFF;             // blank byte; the geometry's, unless saves_set_blank overrode it
static SemaphoreHandle_t sv_mutex, sv_blk_sem;
static TaskHandle_t sv_task;
static volatile uint16_t sv_rx_blk = 0xFFFF;
// Set only by the FPGA's 0x0B notice. Gates every dump, so a core without the
// save channel (or an old bitstream) is never asked for blocks it cannot send.
static volatile bool sv_dirty = false;
// The ROM loaded by the core's loader: iNES has no way to tell us at set_game
// time, so loadnes() calls saves_set_battery() once it has read the header.
// False: the WRAM under $6000-$7FFF is volatile scratch — restore still wipes
// it with the blank image (no previous game's save may leak in), but no dump
// ever creates a file.
static volatile bool sv_battery = true;
// Dumps are armed only while the game runs on screen (overlay off). Menus and
// loads settle it first. See the FatFs note above.
static volatile bool sv_live = false;

// What happened to this game's saves, for saves_status_text(). The counters
// run since boot.
enum { SV_NEVER, SV_WROTE, SV_SAME, SV_FAILED };
static volatile uint8_t sv_last = SV_NEVER;     // result of the last dump of this game
static const char *volatile sv_why = "";        // why it failed (static string)
static volatile uint32_t sv_last_ms;            // when sv_last was set
static uint32_t sv_last_dump_ms;                // when the last dump (any game) started
static uint32_t sv_n_wrote, sv_n_same, sv_n_failed, sv_n_noreply, sv_n_badframe;
// The core has answered or announced on the save channel (a 0x0A block or a
// 0x0B notice) since this core was last programmed. Only then does a menu
// opening verify the card against the RAM without a notice: an old bitstream
// without the channel would cost a 1 s timeout every time.
static volatile bool sv_chan_seen = false;
static uint16_t sv_chan_core = 0xFFFF;          // the core it was seen on

#define SV_DEBOUNCE_MS 1000         // from the first write to the dump
#define SV_BUSY_DEBOUNCE_MS 4000    // ... when the last dump was less than SV_BUSY_MS ago
#define SV_BUSY_MS 10000
#define SV_RETRY_MS 3000
#define SV_RETRIES 5
#define SV_TASK_STACK 1536          // words; FatFs puts its 255-char name buffers (exFAT: ~1.1 KB per call) on the stack
#define SV_VERIFY_MAX_BLOCKS 32     // a menu opening verifies a game this small against the card

// ---- RX task hooks (must stay cheap: no SD I/O here) ----
void saves_rx_byte(uint16_t blk, uint16_t off, uint8_t b) {
    if (off < SV_BLK)
        sv_rx[off] = b;
    (void)blk;                              // one save channel; block checked at done
}
void saves_rx_block_done(uint16_t blk) {
    sv_chan_seen = true;
    sv_rx_blk = blk;
    if (sv_blk_sem) xSemaphoreGive(sv_blk_sem);
}
void saves_rx_dirty(void) {
    sv_chan_seen = true;
    sv_dirty = true;
    if (sv_live && sv_task)                 // menu up: the settle flushes it instead
        xTaskNotifyGive(sv_task);
}
void saves_rx_bad_frame(void) {
    sv_n_badframe++;                        // a block frame that was cut short or mis-sized
}

// ---- FPGA side ----
static void sv_send_block(uint16_t blk, const uint8_t *data) {      // 0x11
    taskENTER_CRITICAL();                   // don't interleave with other senders
    fpga_tx_header(0x11, 1 + 2 + SV_BLK);
    fpga_tx_byte(blk >> 8);
    fpga_tx_byte(blk & 0xff);
    for (uint16_t i = 0; i < SV_BLK; i++)
        fpga_tx_byte(data[i]);
    taskEXIT_CRITICAL();
    heartbeat_bump();
}

// Read one block back from the FPGA into sv_rx. False if it doesn't answer.
// quiet: no failure bookkeeping (the caller expects old bitstreams not to answer).
static bool sv_fetch_block(uint16_t blk, uint32_t timeout_ms = 1000, bool quiet = false) {  // 0x12 -> sv_rx
    xSemaphoreTake(sv_blk_sem, 0);                                  // drop a stale give
    taskENTER_CRITICAL();
    fpga_tx_header(0x12, 3);
    fpga_tx_byte(blk >> 8);
    fpga_tx_byte(blk & 0xff);
    taskEXIT_CRITICAL();
    if (xSemaphoreTake(sv_blk_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE || sv_rx_blk != blk) {
        if (!quiet) {
            sv_n_noreply++;
            sv_why = "no reply";
            dprint("saves: block %u timed out (frames bad %u, rx overflow %d), save NOT written",
                   blk, (unsigned)sv_n_badframe, (int)uart_rx_fifo_overflowed());
        }
        return false;
    }
    heartbeat_bump();                       // a whole dump takes a while
    return true;
}

// ---- flush: dump the core's RAM to sv_path. Called with sv_mutex held ----
static bool sv_dump_and_write(void);

// Records the result for saves_status_text(). Returns true if the dump
// succeeded (written, or identical to the file).
static bool sv_flush_locked(void) {
    if (!sv_geom || !sv_path[0]) return true;
    if (!sv_battery) {                      // volatile scratch: never make a file
        sv_dirty = false;
        return true;
    }
    // Pause a game that shares the save RAM's port, so the dump can't delay or
    // disturb its accesses. Afterwards recompute the bits rather than restore
    // them: a menu may have opened (and paused the game) meanwhile.
    if (sv_geom->pause_dump)
        set_core_config(get_core_config() | CORE_CFG_MENU_PAUSE);
    sv_last_dump_ms = (uint32_t)bflb_mtimer_get_time_ms();
    bool ok = sv_dump_and_write();
    if (sv_geom->pause_dump)
        apply_core_config();
    sv_last_ms = (uint32_t)bflb_mtimer_get_time_ms();
    if (!ok) {
        sv_last = SV_FAILED;
        sv_n_failed++;
    }
    return ok;
}

static bool sv_dump_and_write(void) {
    // Clear BEFORE the dump: the FPGA clears its flag when block 0 is requested,
    // and a write that lands during the dump sends a fresh 0x0B that sets this
    // again, so a save can lag but cannot be silently lost.
    sv_dirty = false;

    // Pass 1: does the image differ from what the file (or a blank RAM) holds?
    UINT br = 0;
    bool opened = (f_open(&fsave, sv_path, FA_READ) == FR_OK);
    bool changed = !opened;                 // nothing to compare against: dump again
    bool ok = true;
    for (uint16_t blk = 0; blk < sv_blocks && ok; blk++) {
        if (!sv_fetch_block(blk)) { ok = false; break; }
        if (opened) {
            if (f_read(&fsave, sv_io, SV_BLK, &br) != FR_OK || br != SV_BLK) {
                memset(sv_io, sv_blank, SV_BLK);              // short file: blank
                changed = true;
            }
        } else
            memset(sv_io, sv_blank, SV_BLK);
        if (memcmp(sv_io, sv_rx, SV_BLK) != 0)
            changed = true;
    }
    if (opened) f_close(&fsave);
    if (!ok) { sv_dirty = true; return false; }                     // try again later
    if (!changed) {
        sv_last = SV_SAME;
        sv_n_same++;
        return true;                                                // no SD write
    }

    // Pass 2: dump again straight into <file>.sav.tmp (the RAM may even have
    // changed since pass 1; that only re-dirties us).
    FRESULT fr = f_mkdir(sv_root);                                  // FR_EXIST is fine
    if (fr != FR_OK && fr != FR_EXIST) {
        dprint("saves: mkdir %s failed (%d)", sv_root, (int)fr);
        sv_why = "mkdir"; sv_dirty = true; return false;
    }
    fr = f_mkdir(sv_dir);
    if (fr != FR_OK && fr != FR_EXIST) {
        dprint("saves: mkdir %s failed (%d)", sv_dir, (int)fr);
        sv_why = "mkdir"; sv_dirty = true; return false;
    }
    char tmp[270]; snprintf(tmp, sizeof tmp, "%s.tmp", sv_path);
    UINT bw = 0;
    fr = f_open(&fsave, tmp, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK) {
        dprint("saves: cannot create %s (%d)", tmp, (int)fr);
        sv_why = "create file"; sv_dirty = true; return false;
    }
    for (uint16_t blk = 0; blk < sv_blocks && ok; blk++) {
        if (!sv_fetch_block(blk)) { ok = false; break; }
        if (f_write(&fsave, sv_rx, SV_BLK, &bw) != FR_OK || bw != SV_BLK) {
            sv_why = "card write";
            ok = false;
        }
    }
    bool synced = (f_sync(&fsave) == FR_OK);
    bool closed = (f_close(&fsave) == FR_OK);
    if (ok && !(synced && closed)) { sv_why = "card sync"; ok = false; }
    if (ok) {
        f_unlink(sv_path);                    // FatFs rename refuses an existing target
        fr = f_rename(tmp, sv_path);
        if (fr != FR_OK) {
            dprint("saves: rename to %s failed (%d)", sv_path, (int)fr);
            sv_why = "rename";
            ok = false;
        } else {
            dprint("saves: wrote %s", sv_path);
            sv_last = SV_WROTE;
            sv_n_wrote++;
        }
    }
    if (!ok) sv_dirty = true;                                       // retry next time
    return ok;
}

// ---- API ----
void saves_set_game(const char *fname, uint16_t core_id) {
    if (!sv_mutex) return;
    saves_settle();                         // flush the outgoing game under its own name
    if (xSemaphoreTake(sv_mutex, portMAX_DELAY) != pdTRUE) return;
    sv_geom = NULL;
    sv_battery = true;                      // loaders without a battery flag (SMS) default to saving
    sv_path[0] = '\0';
    for (unsigned i = 0; i < N_GEOMS; i++) {
        if (save_geoms[i].core_id == core_id) { sv_geom = &save_geoms[i]; break; }
    }
    sv_blocks = sv_geom ? sv_geom->blocks : 0;  // SNES: the loader refines this
    sv_blank = sv_geom ? sv_geom->blank : 0xFF; // per-game override dies with the game
    if (sv_geom) {
        // Drive prefix from the path itself ("sd:" / "usb:"), else the mounted drive.
        const char *colon = strchr(fname, ':');
        char prefix[8] = "";
        if (colon && colon - fname < (int)sizeof prefix - 1) {
            memcpy(prefix, fname, colon - fname + 1);
            prefix[colon - fname + 1] = 0;
        } else
            snprintf(prefix, sizeof prefix, "%s", drv);
        const char *base = strrchr(fname, '/');
        base = base ? base + 1 : (colon ? colon + 1 : fname);
        char name[200];
        snprintf(name, sizeof name, "%s", base);
        char *dot = strrchr(name, '.');
        if (dot) *dot = 0;                  // <rom basename without extension>.sav
        snprintf(sv_root, sizeof sv_root, "%ssaves", prefix);
        snprintf(sv_dir, sizeof sv_dir, "%ssaves/%s", prefix, sv_geom->dir);
        snprintf(sv_path, sizeof sv_path, "%s/%s.sav", sv_dir, name);
        dprint("saves: game set, save file %s", sv_path);
    }
    sv_dirty = false;                       // the old game's pending state dies here
    sv_live = false;
    sv_last = SV_NEVER;                     // nothing has happened to this game's save yet
    sv_why = "";
    if (core_id != sv_chan_core) {          // another core: its channel is unknown again
        sv_chan_seen = false;
        sv_chan_core = core_id;
    }
    xSemaphoreGive(sv_mutex);
}

// The per-game geometry the ROM loader read out of the ROM itself (the SNES
// header's SRAM size byte: 2^n KB, 0 = none). Call after saves_set_game,
// before saves_restore; until then the geometry's own block count (0 for such
// per-game cores) keeps the engine passive.
void saves_set_blocks(uint16_t blocks) {
    if (!sv_mutex) return;
    if (xSemaphoreTake(sv_mutex, portMAX_DELAY) != pdTRUE) return;
    if (sv_geom) sv_blocks = blocks;        // no game (or a fixed-geometry core): ignored
    xSemaphoreGive(sv_mutex);
}

void saves_set_blank(uint8_t blank) {
    sv_blank = blank;
}

void saves_restore(void) {
    if (!sv_mutex) return;
    if (xSemaphoreTake(sv_mutex, portMAX_DELAY) != pdTRUE) return;
    if (!sv_geom || !sv_path[0]) { xSemaphoreGive(sv_mutex); return; }
    char tmp[270]; snprintf(tmp, sizeof tmp, "%s.tmp", sv_path);
    const char *src = "blank";
    bool opened = sv_battery && f_open(&fsave, sv_path, FA_READ) == FR_OK;
    if (!opened && sv_battery && f_open(&fsave, tmp, FA_READ) == FR_OK) {   // power cut mid-write
        opened = true;
        src = "recovered .tmp";
    }
    // Always send SOMETHING, every block: the save RAM may still hold the
    // previous game's data if the bitstream was not reprogrammed between games.
    for (uint16_t blk = 0; blk < sv_blocks; blk++) {
        UINT br = 0;
        if (!opened || f_read(&fsave, sv_io, SV_BLK, &br) != FR_OK || br != SV_BLK)
            memset(sv_io, sv_blank, SV_BLK);
        sv_send_block(blk, sv_io);
    }
    if (opened) f_close(&fsave);
    // The FPGA announces a write (0x0B) only when its dirty flag goes from
    // clean to dirty, and clears the flag only when block 0 is requested. A
    // notice we dropped for the previous game (a ROM without battery RAM, or a
    // write between set_game and now) left the flag set, and this game's writes
    // would never be announced. Reading block 0 -- the core is still held, so
    // nothing is dirty but us -- makes the flag match our "clean". An old
    // bitstream does not answer: a short wait.
    if (sv_blocks && sv_fetch_block(0, 150, true))
        sv_chan_seen = true;
    sv_dirty = false;
    sv_live = false;
    xSemaphoreGive(sv_mutex);
    dprint("saves: restored from %s", src);
}

// Core loaders: does THIS ROM have battery-backed save RAM? NES calls it from
// its loader after reading the iNES header; the geometry's core default is
// true. False drops any pending dump and skips the file on restore (the blank
// wipe still runs, see saves_restore). Call before saves_restore().
void saves_set_battery(bool battery) {
    sv_battery = battery;
    if (!battery)
        sv_dirty = false;
    dprint("saves: battery %s", battery ? "present" : "absent");
}

void saves_settle(void) {
    if (!sv_mutex) return;
    if (xSemaphoreTake(sv_mutex, portMAX_DELAY) != pdTRUE) return;
    sv_live = false;                        // no new dumps after this point
    // A notice can be lost (or never sent, if the FPGA thought it had already
    // told us), so a small save is checked against the card even when nothing
    // is marked dirty: it costs ~16 blocks of UART and writes nothing if the
    // card is current. The status line then says "up to date".
    bool verify = sv_chan_seen && sv_geom && sv_path[0] && sv_battery &&
                  sv_blocks && sv_blocks <= SV_VERIFY_MAX_BLOCKS;
    if (sv_dirty || verify)
        sv_flush_locked();
    xSemaphoreGive(sv_mutex);
}

void saves_off(void) {
    saves_settle();
    if (xSemaphoreTake(sv_mutex, portMAX_DELAY) != pdTRUE) return;
    sv_geom = NULL;
    sv_path[0] = '\0';                      // a stray 0x0B can no longer reach a file
    xSemaphoreGive(sv_mutex);
}

void saves_rearm(void) {
    if (!sv_mutex) return;
    if (xSemaphoreTake(sv_mutex, portMAX_DELAY) != pdTRUE) return;
    sv_live = true;
    if (sv_dirty && sv_task)                // wrote while we were disarmed
        xTaskNotifyGive(sv_task);
    xSemaphoreGive(sv_mutex);
}

static void saves_task(void *pvParameters) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);                    // the game wrote save RAM
        // Debounce: wait out the burst, so one save covers it. A game that was
        // dumped moments ago is writing continuously (save RAM as work RAM):
        // wait longer, so the card is not rewritten every second.
        uint32_t since = (uint32_t)bflb_mtimer_get_time_ms() - sv_last_dump_ms;
        uint32_t wait = (sv_last_dump_ms && since < SV_BUSY_MS) ? SV_BUSY_DEBOUNCE_MS : SV_DEBOUNCE_MS;
        while (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait)) > 0) {}
        for (unsigned attempt = 0;; attempt++) {
            bool ok = true;
            if (xSemaphoreTake(sv_mutex, portMAX_DELAY) == pdTRUE) {
                if (sv_live && sv_dirty)    // a settle may have flushed it meanwhile
                    ok = sv_flush_locked();
                xSemaphoreGive(sv_mutex);
            }
            if (ok || attempt >= SV_RETRIES)
                break;
            vTaskDelay(pdMS_TO_TICKS(SV_RETRY_MS));                 // then try again
        }
    }
}

// One line for the game menu (<= 30 columns), or "" if this core has no
// battery saves. Reads state without the mutex: a stale word on screen is
// harmless, and a dump in flight must not block the menu.
void saves_status_text(char *buf, size_t n) {
    buf[0] = '\0';
    if (!sv_geom || !sv_path[0])
        return;
    if (!sv_battery) {
        snprintf(buf, n, "No battery save in this ROM");
        return;
    }
    if (!sv_blocks) {
        snprintf(buf, n, "No save RAM in this ROM");
        return;
    }
    uint32_t age = ((uint32_t)bflb_mtimer_get_time_ms() - sv_last_ms) / 1000;
    char ago[12];
    if (age < 100)
        snprintf(ago, sizeof ago, "%us", (unsigned)age);
    else if (age < 6000)
        snprintf(ago, sizeof ago, "%um", (unsigned)(age / 60));
    else
        snprintf(ago, sizeof ago, "%uh", (unsigned)(age / 3600));
    if (sv_last == SV_FAILED)
        snprintf(buf, n, "Save FAILED: %s", sv_why);
    else if (sv_dirty && sv_last != SV_NEVER)
        snprintf(buf, n, "Save: waiting to write");
    else if (sv_last == SV_WROTE)
        snprintf(buf, n, "Saved to card %s ago", ago);
    else if (sv_last == SV_SAME)
        snprintf(buf, n, "Save on card is current");
    else if (sv_dirty)
        snprintf(buf, n, "Save: waiting to write");
    else
        snprintf(buf, n, "Save: none written yet");
}

// Counters for the diagnostic line (<= 32 columns): dumps written, found
// current, failed; blocks the FPGA never answered; block frames that arrived
// cut short; an "O" if the UART hardware FIFO ever overflowed.
void saves_diag_text(char *buf, size_t n) {
    snprintf(buf, n, "SV w%u s%u f%u n%u b%u %s%s",
             (unsigned)sv_n_wrote, (unsigned)sv_n_same, (unsigned)sv_n_failed,
             (unsigned)sv_n_noreply, (unsigned)sv_n_badframe,
             uart_rx_fifo_overflowed() ? "O" : "-", sv_chan_seen ? "C" : "-");
}

void saves_init(void) {
    sv_mutex = xSemaphoreCreateMutex();
    sv_blk_sem = xSemaphoreCreateBinary();
    if (xTaskCreate(saves_task, "saves", SV_TASK_STACK, NULL, 1, &sv_task) != pdPASS) {
        sv_task = NULL;                 // no async dumps; menu/load flushes still work
        dprint("saves: task not created (heap?), async saving OFF");
    }
}
