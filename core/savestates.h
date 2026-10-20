#pragma once

#include <cstdint>

// Save states: full machine snapshots in `saves/<geom>/<rom>.ss1`..`ss4`,
// beside the battery `.sav` (see saves.h, which this module does not touch).
// Wire protocol (the FPGA side is the core's savestate engine behind
// iosys_bl616's SS channel):
//
//   MCU -> FPGA 0x18 op                    0 query, 1 begin-save, 2 begin-load, 3 end, 4 abort
//   MCU -> FPGA 0x19 blk[15:0] <512 bytes> load one state block
//   MCU -> FPGA 0x1A blk[15:0]             request one state block
//   FPGA -> MCU 0x0E <16 bytes>            status (answers every 0x18)
//   FPGA -> MCU 0x0F blk[15:0] <512 bytes> the requested state block
//
// File format (512-byte blocks): block 0 is a header the FPGA generates and
// the firmware patches before commit -- magic "TCSS", versions, core id,
// mapper_flags, section sizes, then firmware fields (ROM size + CRC32,
// payload CRC32 over blocks 1..). A load re-checks every field against the
// running game before the FPGA is touched, and the core checks again, so a
// state for another game or another core build is a clean menu error.
//
// Threading mirrors saves.cpp: transfers run in main_task from the game
// menu (after saves_settle(), so no battery dump is in flight and this is
// the only FatFs user); the UART RX task only assembles status/block frames
// through the savestates_rx_* hooks.

enum savestate_result {
    SS_OK = 0,
    SS_UNSUPPORTED,     // core build or mapper has no savestates
    SS_NO_GAME,         // no game / no slot path (menu opened without a game)
    SS_EMPTY,           // load: slot has no state file
    SS_WRONG_GAME,      // load: header does not match the running game/build
    SS_CORRUPT,         // load: payload CRC mismatch / short file
    SS_SD_ERROR,        // FatFs failure (incl. card full)
    SS_LINK_ERROR,      // UART timeout mid-transfer (game may need a reset)
};

// Game lifecycle, called next to the saves_* equivalents. set_game only
// records paths (no file I/O, no link traffic); probe queries the running
// core once (250 ms timeout on cores that never answer) and caches the
// result. set_rom_id records the loaded ROM's identity from the loader
// that streamed it (NES and SMS so far; other cores skip it, so loads of
// their states check flags/size but not ROM identity).
void savestates_set_game(const char *rom_path, int core_id);
void savestates_set_rom_id(uint32_t size, uint32_t crc32);
void savestates_probe(void);
void savestates_off(void);
void savestates_discard(void);

// Menu queries/actions (main_task). Slots are 1..4. slot_size returns the
// state file size, 0 when empty.
bool savestates_supported(void);
uint32_t savestates_slot_size(int slot);
savestate_result savestates_save(int slot);
savestate_result savestates_load(int slot);
// True when the last load got far enough that the core was reset from the
// state (a failure after that point leaves the machine indeterminate; the
// menu resets the game). Header-phase rejects never set it.
bool savestates_last_load_disturbed(void);
const char *savestate_result_str(savestate_result r);

// UART1 RX task hooks (main.cpp), model after the saves_rx_* ones.
void savestates_rx_status(const uint8_t payload[16]);
void savestates_rx_byte(uint16_t blk, int idx, uint8_t b);
void savestates_rx_block_done(uint16_t blk);
void savestates_rx_bad_frame(void);
