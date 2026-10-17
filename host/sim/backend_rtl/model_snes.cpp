// SnesModel: RtlModel for the SNES cosim_top (snestang sim/cosim/).
//
// Core-specific facts encapsulated here (the recipe for the next core lists
// them): the Verilated header names (prefix Vsnestang_cosim, so a binary
// linking two RTL cores sees two distinct model classes), the hierarchical
// paths of the OSD text array (iosys sys -> textdisp disp -> DPB menu_mem ->
// mem) and the SDRAM array (chip -> mem), the save window base (BSRAM at
// linear 0xF00000, bank 1 word 0x780000 per sdram_cl2_2ch), and the clock
// ratio (fclk = 3x clk, coincident rising edges; clk is the SNES mclk).
//
// Stepping: one clk period is 6 evals + the initial settle (seven evals per
// tick, every edge) -- the straightforward clocking, identical to the NES
// model. Fewer (rising edges only) breaks the save path in practice, so the
// extra evals stay. hclk mirrors clk: it only feeds textdisp's render
// pipeline, whose pixels nobody observes.
#include <atomic>
#include "rtl_model.hpp"

#include "Vsnestang_cosim.h"
#include "Vsnestang_cosim_cosim_top.h"
#include "Vsnestang_cosim_gowin_dpb_menu.h"
#include "Vsnestang_cosim_iosys_bl616_cosim__pi1.h"
#include "Vsnestang_cosim_sdram_chip.h"
#include "Vsnestang_cosim_textdisp__Cz1.h"
#include "verilated.h"

namespace {

class SnesModel : public RtlModel {
  public:
    SnesModel() {
        Verilated::debug(0);
        top.clk = 0;
        top.fclk = 0;
        top.hclk = 0;
        top.resetn = 0;
        top.joy1 = 0;
        top.joy2 = 0;
        top.uart_rx = 1;
        top.silence = 0;
        top.cosim_core_id = 2;
        top.poke_valid = 0;
        top.poke_off = 0;
        top.poke_data = 0;
        top.churn_en = 0;
        top.eval();
    }

    void step(uint64_t n, RtlPins &pins, uint8_t *tx_levels) override {
        top.joy1 = pins.joy1;
        top.joy2 = pins.joy2;
        top.uart_rx = pins.uart_rx ? 1 : 0;
        top.silence = pins.silence ? 1 : 0;
        top.resetn = pins.resetn ? 1 : 0;
        top.cosim_core_id = pins.core_id;
        top.poke_valid = pins.poke_valid ? 1 : 0;
        top.poke_off = pins.poke_off;
        top.poke_data = pins.poke_data;
        top.churn_en = pins.churn ? 1 : 0;
        // Seven evals per tick (every edge): the straightforward clocking.
        for (uint64_t i = 0; i < n; i++) {
            top.clk = 0;
            top.fclk = 0;
            top.eval();
            top.fclk = 1;
            top.eval();
            top.fclk = 0;
            top.eval();
            top.clk = 1;
            top.fclk = 1;
            top.hclk = 1;
            top.eval();
            if (tx_levels)
                tx_levels[i] = top.uart_tx ? 1 : 0;
            top.fclk = 0;
            top.eval();
            top.fclk = 1;
            top.eval();
            top.clk = 0;
            top.fclk = 0;
            top.hclk = 0;
            top.eval();
        }
        pins.uart_tx = top.uart_tx != 0;
        pins.tx_pending = top.tx_pending != 0;
        pins.core_config = top.core_config;
        pins.video_config = top.video_config;
        pins.overlay = top.overlay != 0;
        pins.busy = top.sdram_busy != 0;
        pins.rom_bytes = top.rom_bytes;
        pins.poke_ack = top.poke_ack != 0;
    }

    uint8_t osd_byte(int row, int col) override {
        // DPB address is {1'b0, y[4:0], x[4:0]} (see textdisp.v).
        return top.cosim_top->sys->disp->menu_mem->mem[row * 32 + col];
    }

    uint8_t save_byte(uint32_t linear_addr) override {
        // Save RAM = the BSRAM window: bank 1, linear word 0x780000+
        // (base 0xF00000 in bytes). 24-bit word index into the chip array.
        uint32_t w = (linear_addr >> 1) & ((1 << 24) - 1);
        uint8_t hi = (linear_addr & 1) ? 1 : 0;
        uint16_t word = top.cosim_top->chip->mem[w];
        return hi ? (uint8_t)(word >> 8) : (uint8_t)(word & 0xff);
    }

    const char *name() override {
        return "snestang-rtl";
    }

    uint32_t save_base() override {
        return 0xF00000; // SNES BSRAM window (linear), as sdram_cl2_2ch maps it
    }

    Vsnestang_cosim top;
};

} // namespace

// Factory used by backend_rtl.cpp (declared there).
RtlModel *new_rtl_model();
RtlModel *new_rtl_model() {
    return new SnesModel();
}
