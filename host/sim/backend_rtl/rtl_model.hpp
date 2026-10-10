// RtlModel: the per-core side of the RTL backend (see backend_rtl.cpp).
//
// The shared bridge (backend_rtl.cpp) speaks only this interface, so adding
// another core means writing a new model_<core>.cpp against that core's
// Verilated cosim_top plus a cosim_top.sv following the same contract --
// backend_rtl.cpp itself stays untouched. Verilated model objects are only
// touched from run_until (always under the sim scheduler lock), so no
// locking is needed inside the implementation; queue/thread safety lives in
// the bridge.
//
// Units: ticks (one sim tick = one clk period = 1/21.492MHz; the model
// clocks fclk=3x clk internally per step_tick).
#pragma once

#include <cstdint>

struct RtlPins {
    // Bridge -> model (applied at the next step_tick).
    uint16_t joy1 = 0, joy2 = 0;
    bool uart_rx = true;
    bool silence = false;     // MODE blackout: gate both UART directions
    bool resetn = true;
    uint16_t core_id = 1;     // programmed core ID (programming model)
    bool poke_valid = false;  // game-path WRAM write request
    uint16_t poke_off = 0;    // offset into the save window (bytes)
    uint8_t poke_data = 0;
    bool poke_ack_seen = false; // (bridge-side bookkeeping, ignored by model)
    bool churn = false;         // game continuously scribbles WRAM

    // Model -> bridge (refreshed by step_tick).
    bool uart_tx = true;
    bool tx_pending = true; // a reply is owed or a frame is on the wire
    uint32_t core_config = 0;
    uint32_t video_config = 0;
    bool overlay = true;
    bool busy = true; // SDRAM controller still initializing
    uint32_t rom_bytes = 0;
    bool poke_ack = false;
};

class RtlModel {
  public:
    virtual ~RtlModel() = default;
    virtual const char *name() = 0;      // backend name, e.g. "nestang-rtl"
    virtual uint32_t save_base() = 0;    // linear SDRAM addr of save window
    // Apply `pins` inputs, advance exactly `n` clk ticks, refresh `pins`
    // outputs and record uart_tx per tick into tx_levels (n entries).
    // No sim:: calls inside (scheduler lock is held).
    virtual void step(uint64_t n, RtlPins &pins, uint8_t *tx_levels) = 0;
    // OSD character cell (row 0-27, col 0-31) and save-window byte (linear
    // SDRAM byte address). Plain array reads outside stepping: a torn read
    // just delays a script poll by one round, like sampling real hardware
    // mid-frame.
    virtual uint8_t osd_byte(int row, int col) = 0;
    virtual uint8_t save_byte(uint32_t linear_addr) = 0;
};
