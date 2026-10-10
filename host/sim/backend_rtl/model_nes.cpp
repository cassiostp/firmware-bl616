// NesModel: RtlModel for the NES cosim_top (nestang sim/cosim/).
//
// Core-specific facts encapsulated here (the recipe for the next core lists
// them): the Verilated header names, the hierarchical paths of the OSD text
// array (iosys sys -> textdisp disp -> DPB menu_mem -> mem) and the SDRAM
// array (chip -> mem), the save window base (linear 0x3C0000), and the clock
// ratio (fclk = 3x clk, coincident rising edges).
//
// Stepping: one clk period is 6 evals (fclk toggles 3x per clk, clk rising
// coincident with an fclk rise, like the board PLL). hclk mirrors clk: it
// only feeds textdisp's render pipeline, whose pixels nobody observes.
#include <atomic>
#include "rtl_model.hpp"

#include "Vcosim_top.h"
#include "Vcosim_top_cosim_top.h"
#include "Vcosim_top_gowin_dpb_menu.h"
#include "Vcosim_top_iosys_bl616_cosim__pi1.h"
#include "Vcosim_top_sdram_chip.h"
#include "Vcosim_top_textdisp.h"
#include "verilated.h"

namespace {

class NesModel : public RtlModel {
  public:
    NesModel() {
        Verilated::debug(0);
        top.clk = 0;
        top.fclk = 0;
        top.hclk = 0;
        top.resetn = 0;
        top.joy1 = 0;
        top.joy2 = 0;
        top.uart_rx = 1;
        top.silence = 0;
        top.cosim_core_id = 1;
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
        // Fewer (rising edges only) breaks the save path in practice, so the
        // extra evals stay.
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
        uint32_t w = linear_addr >> 1;
        uint8_t hi = (linear_addr & 1) ? 1 : 0;
        uint16_t word = top.cosim_top->chip->mem[w & ((1 << 21) - 1)];
        return hi ? (uint8_t)(word >> 8) : (uint8_t)(word & 0xff);
    }

    const char *name() override {
        return "nestang-rtl";
    }

    uint32_t save_base() override {
        return 0x3C0000; // NES WRAM window (linear), as nestang_top maps it
    }

    Vcosim_top top;
};

} // namespace

// Factory used by backend_rtl.cpp (declared there).
RtlModel *new_rtl_model();
RtlModel *new_rtl_model() {
    return new NesModel();
}
