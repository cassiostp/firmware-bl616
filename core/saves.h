// Battery saves: the cores' save RAM <-> SD card. See saves.cpp.
#pragma once

#include <stdint.h>

void saves_init(void);              // create the save task; call once at boot
// The game about to be loaded: names the save file, and picks the geometry by
// core id (no geometry: the engine goes passive). The previous game's pending
// save is flushed first, while the FPGA still holds it.
void saves_set_game(const char *fname, uint16_t core_id);
void saves_restore(void);           // send the save into the FPGA; the core must not run yet
// Settle any pending dump, then stop async dumps until re-armed: call when a
// menu opens over the running game, and before the FPGA stops running it.
void saves_settle(void);
// Settle, then forget the game entirely: call before closing a game or
// programming a bitstream that replaces it, never after.
void saves_off(void);
void saves_rearm(void);             // the game runs again: dirty notices matter again

// Called from uart1_rx_task only.
void saves_rx_byte(uint16_t blk, uint16_t off, uint8_t b);
void saves_rx_block_done(uint16_t blk);
void saves_rx_dirty(void);
