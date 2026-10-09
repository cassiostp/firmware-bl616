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
//   - 2 s after the game's last save-RAM write (the 0x0B notice, debounced by a
//     FreeRTOS task). This covers a burst of writes with one SD write.
//   - when a menu opens over the running game, and before a different ROM/core
//     loads or the game closes/resets: saves_settle() flushes synchronously.
//   - never during a game switch: the old game's pending save is flushed first,
//     under the old file's name, then dropped with it. The new game's RAM can
//     never be dumped under the old name.
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
}
#include <string.h>
#include <stdio.h>

#include "utils.h"
#include "overlay.h"

// Per-core save geometry. Blocks are 512 bytes, like the wire protocol.
struct save_geom {
    uint16_t core_id;
    uint16_t blocks;
    const char *dir;        // folder under <drive>saves/
    uint8_t blank;          // byte an empty save RAM comes up as
};
static const save_geom save_geoms[] = {
    // SMS: the core's 32 KB nvram. MiSTer's SMS core (SMS_MiSTer) keeps the
    // same layout, raw and interchangeable: its backup RAM is a dpram
    // widthad_a=15 (SMS.sv), saved 64 x 512 bytes straight to <rom>.sav, and
    // its empty-RAM init file (rtl/nvram_ff.mif) is all 0xFF, not 0x00.
    {5, 64, "sms", 0xFF},
    // SNES: the block count is per game, from the ROM header's SRAM size byte
    // (saves_set_blocks; 0 = the game has no battery RAM). Two of MiSTer's
    // SNES core (SNES_MiSTer) details carry over: the backup RAM is dumped
    // raw as 512-byte sectors ({sd_lba, sd_buff_addr} address its dpram
    // directly, SNES.sv), a fresh/blank BSRAM is 0xFF -- the core "thrashes"
    // it with 8'hFF on every ROM load (data_a = clearing_ram ? 8'hFF, SNES.sv)
    // -- and battery saves are gated on the header's RAM-size byte being
    // non-zero (bk_ena <= |ram_mask).
    {2, 0, "snes", 0xFF},
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
static SemaphoreHandle_t sv_mutex, sv_blk_sem;
static TaskHandle_t sv_task;
static volatile uint16_t sv_rx_blk = 0xFFFF;
// Set only by the FPGA's 0x0B notice. Gates every dump, so a core without the
// save channel (or an old bitstream) is never asked for blocks it cannot send.
static volatile bool sv_dirty = false;
// Dumps are armed only while the game runs on screen (overlay off). Menus and
// loads settle it first. See the FatFs note above.
static volatile bool sv_live = false;

// ---- RX task hooks (must stay cheap: no SD I/O here) ----
void saves_rx_byte(uint16_t blk, uint16_t off, uint8_t b) {
    if (off < SV_BLK)
        sv_rx[off] = b;
    (void)blk;                              // one save channel; block checked at done
}
void saves_rx_block_done(uint16_t blk) {
    sv_rx_blk = blk;
    if (sv_blk_sem) xSemaphoreGive(sv_blk_sem);
}
void saves_rx_dirty(void) {
    sv_dirty = true;
    if (sv_live && sv_task)                 // menu up: the settle flushes it instead
        xTaskNotifyGive(sv_task);
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
static bool sv_fetch_block(uint16_t blk) {                          // 0x12 -> sv_rx
    xSemaphoreTake(sv_blk_sem, 0);                                  // drop a stale give
    taskENTER_CRITICAL();
    fpga_tx_header(0x12, 3);
    fpga_tx_byte(blk >> 8);
    fpga_tx_byte(blk & 0xff);
    taskEXIT_CRITICAL();
    if (xSemaphoreTake(sv_blk_sem, pdMS_TO_TICKS(1000)) != pdTRUE || sv_rx_blk != blk) {
        dprint("saves: block %u timed out, save NOT written", blk);
        return false;
    }
    heartbeat_bump();                       // a whole dump takes a while
    return true;
}

// ---- flush: dump the core's RAM to sv_path. Called with sv_mutex held ----
static void sv_flush_locked(void) {
    if (!sv_geom || !sv_path[0]) return;
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
                memset(sv_io, sv_geom->blank, SV_BLK);              // short file: blank
                changed = true;
            }
        } else
            memset(sv_io, sv_geom->blank, SV_BLK);
        if (memcmp(sv_io, sv_rx, SV_BLK) != 0)
            changed = true;
    }
    if (opened) f_close(&fsave);
    if (!ok) { sv_dirty = true; return; }                           // try again later
    if (!changed) return;                                           // no SD write

    // Pass 2: dump again straight into <file>.sav.tmp (the RAM may even have
    // changed since pass 1; that only re-dirties us).
    f_mkdir(sv_root);                                               // FR_EXIST is fine
    f_mkdir(sv_dir);
    char tmp[270]; snprintf(tmp, sizeof tmp, "%s.tmp", sv_path);
    UINT bw = 0;
    if (f_open(&fsave, tmp, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
        dprint("saves: cannot create %s", tmp);
        sv_dirty = true;
        return;
    }
    for (uint16_t blk = 0; blk < sv_blocks && ok; blk++) {
        if (!sv_fetch_block(blk)) { ok = false; break; }
        if (f_write(&fsave, sv_rx, SV_BLK, &bw) != FR_OK || bw != SV_BLK) ok = false;
    }
    f_sync(&fsave);
    if (f_close(&fsave) != FR_OK) ok = false;
    if (ok) {
        f_unlink(sv_path);                    // FatFs rename refuses an existing target
        if (f_rename(tmp, sv_path) != FR_OK) {
            dprint("saves: rename to %s failed", sv_path);
            ok = false;
        } else
            dprint("saves: wrote %s", sv_path);
    }
    if (!ok) sv_dirty = true;                                       // retry next time
}

// ---- API ----
void saves_set_game(const char *fname, uint16_t core_id) {
    if (!sv_mutex) return;
    saves_settle();                         // flush the outgoing game under its own name
    if (xSemaphoreTake(sv_mutex, portMAX_DELAY) != pdTRUE) return;
    sv_geom = NULL;
    sv_path[0] = '\0';
    for (unsigned i = 0; i < N_GEOMS; i++) {
        if (save_geoms[i].core_id == core_id) { sv_geom = &save_geoms[i]; break; }
    }
    sv_blocks = sv_geom ? sv_geom->blocks : 0;  // SNES: the loader refines this
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
    ulTaskNotifyTake(pdTRUE, 0);            // drop a notify it may have sent
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

void saves_restore(void) {
    if (!sv_mutex) return;
    if (xSemaphoreTake(sv_mutex, portMAX_DELAY) != pdTRUE) return;
    if (!sv_geom || !sv_path[0]) { xSemaphoreGive(sv_mutex); return; }
    char tmp[270]; snprintf(tmp, sizeof tmp, "%s.tmp", sv_path);
    const char *src = "blank";
    bool opened = (f_open(&fsave, sv_path, FA_READ) == FR_OK);
    if (!opened && f_open(&fsave, tmp, FA_READ) == FR_OK) {         // power cut mid-write
        opened = true;
        src = "recovered .tmp";
    }
    // Always send SOMETHING, every block: the save RAM may still hold the
    // previous game's data if the bitstream was not reprogrammed between games.
    for (uint16_t blk = 0; blk < sv_blocks; blk++) {
        UINT br = 0;
        if (!opened || f_read(&fsave, sv_io, SV_BLK, &br) != FR_OK || br != SV_BLK)
            memset(sv_io, sv_geom->blank, SV_BLK);
        sv_send_block(blk, sv_io);
    }
    if (opened) f_close(&fsave);
    sv_dirty = false;
    sv_live = false;
    xSemaphoreGive(sv_mutex);
    dprint("saves: restored from %s", src);
}

void saves_settle(void) {
    if (!sv_mutex) return;
    if (xSemaphoreTake(sv_mutex, portMAX_DELAY) != pdTRUE) return;
    sv_live = false;                        // no new dumps after this point
    if (sv_dirty)
        sv_flush_locked();
    ulTaskNotifyTake(pdTRUE, 0);            // drop a notify we owe nothing for
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
        // Debounce: wait until it has been quiet for 2 s, so one save covers a burst.
        while (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000)) > 0) {}
        if (xSemaphoreTake(sv_mutex, portMAX_DELAY) != pdTRUE) continue;
        if (sv_live && sv_dirty)            // a settle may have flushed it meanwhile
            sv_flush_locked();
        xSemaphoreGive(sv_mutex);
    }
}

void saves_init(void) {
    sv_mutex = xSemaphoreCreateMutex();
    sv_blk_sem = xSemaphoreCreateBinary();
    if (xTaskCreate(saves_task, "saves", 1024, NULL, 1, &sv_task) != pdPASS) {
        sv_task = NULL;                 // no async dumps; menu/load flushes still work
        dprint("saves: task not created (heap?), async saving OFF");
    }
}
