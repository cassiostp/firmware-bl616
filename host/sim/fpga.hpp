// Fake FPGA core: the other end of the firmware's UART1. Forwards to the
// active backend (fake core or RTL model); see backend.hpp.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fpga {

const int OSD_COLS = 32;
const int OSD_ROWS = 28;

struct OsdSnapshot {
    char cells[OSD_ROWS][OSD_COLS];
    int cursor_col = 0;
    int cursor_row = 0;
};

void start();

// Byte queues between the UART shim and this core.
void tx_push(uint8_t b); // MCU -> FPGA
bool rx_available();     // FPGA -> MCU
uint8_t rx_pop();
// The UART receive interrupt: the backends call rx_notify() whenever they have
// put bytes in the FPGA -> MCU queue (after releasing their own locks); the
// firmware's interrupt handler is installed as the hook by bflb_irq_enable.
void set_rx_hook(void (*fn)());
void rx_notify();

// File the "programmer" stub "loaded", and the core ID it reports.
void set_programmed(const char *path);
int programmed_id(); // -1 = nothing programmed / unknown bitstream
std::string programmed_file();
uint64_t ms_since_program(); // sim ms since the last programming (huge if never)

// Last core_config word received (MCU -> FPGA command 0x03).
uint32_t last_config();
// Last video_config word received (MCU -> FPGA command 0x13).
uint32_t last_video_config();

// Last clock frame payload received (MCU -> FPGA command 0x14, 13 bytes:
// unix BE32, BCD datetime[7], solar %, flags). Empty if none seen.
std::vector<uint8_t> last_clock_frame();

// OSD text buffer.
OsdSnapshot osd_snapshot();
uint64_t osd_hash();
std::vector<int> cursor_rows(); // rows with '>' in column 0
bool screen_contains(const std::string &s);

// Save RAM channel (512-byte blocks, like the wire protocol).
void poke_save(uint16_t off, uint8_t val); // write RAM + emit 0x0B dirty
uint8_t save_ram_byte(uint16_t off);
// Game-path writes into save RAM (dirty the save like a running game).
void wram_write(uint16_t off, uint8_t val);
void wram_burst(uint16_t off, uint16_t len, uint8_t seed);

// Pads as the FPGA reads them (change-detect frames reach the firmware).
void set_pads(uint16_t p1, uint16_t p2);

// MODE button: go silent for silence_ms, then answer as core 0 again.
void trigger_mode(int silence_ms);

// Fast ROM loading: the MCU's current link baud (2M unless negotiated up).
void set_baud(uint32_t baud);
uint32_t baud_rate();
uint32_t baud_max();    // highest set_baud seen (latched, for the scripts)
// Fault injection for the fastload tests (RTL only; the fake ignores them).
void corrupt_next_byte();   // flip a bit in the next 0x07 payload byte
void silence_when_rom(uint64_t n, int silence_ms); // MODE once rom_bytes hits n

// Drop the next n core-ID replies (a colliding joypad frame on hardware).
void drop_ids(int n);

// Receiver stuck mid-frame: swallow MCU bytes, stay silent (fake only).
void set_rx_stuck(bool on);

// Raw FPGA->MCU bytes (both backends) and a pad toggle faster than the
// firmware's 50 ms frame gap. Wire faults are RTL-only.
void inject_rx(const uint8_t *b, size_t n);
void set_pad_churn(bool on);
void set_pad_raw(bool on);
void wire_drop(int n);
void wire_flip();
void wire_burst(int n);
void wire_stall(int ms);

// Continuous game WRAM writes (RTL only; fake ignores).
void set_churn(bool on);
void set_save_channel(bool on);
// Save-block requests received so far (wait-dump watches for a new one).
uint64_t save_requests();

bool overlay_visible();
uint64_t rom_bytes();

void set_verbose(bool v);
std::vector<std::string> log_tail(int n);

const char *backend_name();

} // namespace fpga
