// Battery saves: the cores' save RAM <-> SD card. See saves.cpp.
#pragma once

#include <stddef.h>
#include <stdint.h>

void saves_init(void);              // create the save task; call once at boot
// The game about to be loaded: names the save file, and picks the geometry by
// core id (no geometry: the engine goes passive). The previous game's pending
// save is flushed first, while the FPGA still holds it.
void saves_set_game(const char *fname, uint16_t core_id);
// Per-ROM battery flag, for cores whose ROM header says (NES: iNES byte 6 bit
// 1). A geometry's default is "battery present"; call before saves_restore.
void saves_set_battery(bool battery);
// The ROM loader: the game's own save RAM size, in 512-byte blocks (the SNES
// header's SRAM size byte). 0 = no battery RAM. For per-game cores only.
void saves_set_blocks(uint16_t blocks);
void saves_restore(void);           // send the save into the FPGA; the core must not run yet
// Settle any pending dump, then stop async dumps until re-armed: call when a
// menu opens over the running game, and before the FPGA stops running it.
void saves_settle(void);
// Settle, then forget the game entirely: call before closing a game or
// programming a bitstream that replaces it, never after.
void saves_off(void);
// Forget the game WITHOUT flushing: the FPGA no longer holds its save RAM
// (MODE reloaded it), so a flush would only time out block by block.
void saves_discard(void);
void saves_rearm(void);             // the game runs again: dirty notices matter again

// What happened to the running game's save, for the game menu: one line of up
// to ~30 columns ("Saved to card 12s ago", "Save FAILED: no reply", ...), or ""
// if the core has no battery saves. saves_diag_text is the counters, <= 32
// columns, for the diagnostic line.
bool saves_busy(void);              // a dump is talking to the card right now
// The save task's stack high-water mark in words, for the watchdog's
// periodic log snapshot (0 if the task was never created).
unsigned saves_stack_free(void);
void saves_status_text(char *buf, size_t n);
void saves_diag_text(char *buf, size_t n);

// Called from uart1_rx_task only.
void saves_rx_byte(uint16_t blk, uint16_t off, uint8_t b);
void saves_rx_block_done(uint16_t blk);
void saves_rx_dirty(void);
void saves_rx_bad_frame(void);      // a 0x0A block frame arrived cut short or mis-sized
