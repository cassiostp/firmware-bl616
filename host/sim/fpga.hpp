// Fake FPGA core: the other end of the firmware's UART1. Parses MCU frames,
// keeps the OSD text buffer, answers core-ID requests, records core_config,
// and implements the save-RAM channel against an in-memory save RAM.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fpga {

const int OSD_COLS = 32;
const int OSD_ROWS = 28;

void start();

// Byte queues between the UART shim and this core.
void tx_push(uint8_t b);   // MCU -> FPGA
bool rx_available();       // FPGA -> MCU
uint8_t rx_pop();

// File the "programmer" stub "loaded", and the core ID it reports.
void set_programmed(const char *path);
int programmed_id(); // -1 = nothing programmed / unknown bitstream
std::string programmed_file();
uint64_t ms_since_program(); // ms since the last programming (huge if never)

// Last core_config word received (MCU -> FPGA command 0x03).
uint32_t last_config();

// OSD text buffer.
struct OsdSnapshot {
    char cells[OSD_ROWS][OSD_COLS];
    int cursor_col = 0;
    int cursor_row = 0;
};
OsdSnapshot osd_snapshot();
uint64_t osd_hash();
std::vector<int> cursor_rows(); // rows with '>' in column 0
bool screen_contains(const std::string &s);

// Save RAM channel (512-byte blocks, like the wire protocol).
void poke_save(uint16_t off, uint8_t val); // write RAM + emit 0x0B dirty
uint8_t save_ram_byte(uint16_t off);

// MODE button: go silent for silence_ms, then answer as core 0 again.
void trigger_mode(int silence_ms);

bool overlay_visible();
uint64_t rom_bytes();

void set_verbose(bool v);
std::vector<std::string> log_tail(int n);

} // namespace fpga
