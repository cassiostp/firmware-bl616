// FPGA backend interface: the other end of the firmware's UART1.
//
// Two implementations exist: a synchronous fake core (backend_fake.cpp,
// answers instantly in sim time) and a Verilator model of the real NES
// interface logic (backend_rtl.cpp, bit-level UART at the real 2 Mbaud).
// The fpga:: namespace (fpga.hpp) forwards to whichever backend is active,
// so the script runner and tests do not care which one answers.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sim_time.hpp"

struct BackendOsd {
    static const int COLS = 32;
    static const int ROWS = 28;
    char cells[ROWS][COLS];
    int cursor_col = 0;
    int cursor_row = 0;
};

class FpgaBackend : public sim::Backend {
  public:
    // MCU -> FPGA byte (framed-protocol byte from bflb_uart_putchar).
    virtual void tx_push(uint8_t b) = 0;
    // FPGA -> MCU bytes (drained by bflb_uart_getchar/rxavailable).
    virtual bool rx_available() = 0;
    virtual uint8_t rx_pop() = 0;

    virtual void set_programmed(const char *path) = 0;
    virtual int programmed_id() = 0; // -1 = nothing programmed / unknown
    virtual std::string programmed_file() = 0;
    virtual uint64_t ms_since_program() = 0; // sim ms, huge if never

    virtual uint32_t last_config() = 0; // last core_config word (MCU -> core)

    virtual BackendOsd osd_snapshot() = 0;
    virtual uint64_t osd_hash() = 0;
    virtual std::vector<int> cursor_rows() = 0; // rows with '>' in column 0
    virtual bool screen_contains(const std::string &s) = 0;

    // Save-RAM channel (512-byte blocks, like the wire protocol).
    virtual void poke_save(uint16_t off, uint8_t val) = 0; // write RAM + 0x0B
    virtual uint8_t save_ram_byte(uint16_t off) = 0;
    // A game-path write into save RAM: through the real channel on RTL
    // (dirties the save like a running game would), like poke_save on fake.
    virtual void wram_write(uint16_t off, uint8_t val) = 0;
    // len game-path writes of a seeded pattern (seed+i) from off. Models a
    // game scribbling battery RAM as work RAM.
    virtual void wram_burst(uint16_t off, uint16_t len, uint8_t seed) = 0;

    // Pads as the FPGA's own inputs (USB-A pads it reads): change-detect
    // frames reach the firmware like on hardware. p2 is the second pad.
    virtual void set_pads(uint16_t p1, uint16_t p2) = 0;

    // MODE button: silent for silence_ms sim ms, then answer as core 0.
    virtual void trigger_mode(int silence_ms) = 0;

    // Continuous game WRAM writes (combo-during-dump tests). The fake has no
    // game model and ignores it; the RTL traffic generator scribbles WRAM.
    virtual void set_churn(bool on) = 0;
    // Number of 0x12 save-block requests received (wait-dump watches it).
    virtual uint64_t save_requests() = 0;

    virtual bool overlay_visible() = 0;
    virtual uint64_t rom_bytes() = 0; // ROM payload bytes consumed

    virtual void set_verbose(bool v) = 0;
    virtual std::vector<std::string> log_tail(int n) = 0;
    virtual const char *name() = 0;
};

void fpga_use_fake();
void fpga_select(FpgaBackend *b);
FpgaBackend *fpga_active();
// RTL backend factory; null when the sim was built without RTL support, or
// without a model for core_name ("nestang-rtl", "snestang-rtl", ...). The
// first call's name picks the model for the whole process.
FpgaBackend *fpga_rtl_backend(const char *core_name = nullptr);
