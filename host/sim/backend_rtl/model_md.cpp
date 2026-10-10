// MdModel: RtlModel for the MegaDrive cosim_top (mdtang sim/cosim/).
//
// Core-specific facts encapsulated here (the recipe for the next core lists
// them): the Verilated header names, the hierarchical paths of the OSD text
// array (iosys sys -> textdisp disp -> DPB menu_mem -> mem) and the SDRAM
// array (chip -> mem), the save window base (linear 0x820000, cart SRAM at
// SDRAM words {8'h41, 1'b0, X[15:1]}), the big-endian byte lane (even byte =
// word UPPER half, mdtang_top's sv_q mux), and the clocking: sdram.v and
// iosys share ONE clock in mdtang_top, so cosim_top has no fclk and one
// tick = one clk period = one posedge.
//
// Stepping: three evals per tick (low, rising edge, back low) -- the complete
// event set for a design with no falling-edge or multi-clock logic. The NES
// model's seven-eval loop exists for its fclk=3x clkref resync only.
#include <atomic>
#include "rtl_model.hpp"

#include "Vcosim_top.h"
#include "Vcosim_top_cosim_top.h"
#include "Vcosim_top_gowin_dpb_menu.h"
#include "Vcosim_top_iosys_bl616_cosim__pi1.h"
#include "Vcosim_top_sdram_chip.h"
#include "Vcosim_top_textdisp__Cz1.h"
#include "verilated.h"

namespace {

class MdModel : public RtlModel {
  public:
    MdModel() {
        Verilated::debug(0);
        top.clk = 0;
        top.hclk = 0;
        top.resetn = 0;
        top.joy1 = 0;
        top.joy2 = 0;
        top.uart_rx = 1;
        top.silence = 0;
        top.cosim_core_id = 4;
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
        for (uint64_t i = 0; i < n; i++) {
            top.clk = 0;
            top.eval();
            top.clk = 1;
            top.hclk = 1;
            top.eval();
            if (tx_levels)
                tx_levels[i] = top.uart_tx ? 1 : 0;
            top.clk = 0;
            top.hclk = 0;
            top.eval();
        }
        pins.uart_tx = top.uart_tx != 0;
        pins.tx_pending = top.tx_pending != 0;
        pins.core_config = top.core_config;
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
        // Big-endian lane: the EVEN byte of a word is its UPPER half
        // (mdtang_top's sv_q mux / be3 -- the inverse of the NES model).
        uint32_t w = linear_addr >> 1;
        uint16_t word = (uint16_t)top.cosim_top->chip->mem[w & ((1 << 23) - 1)];
        return (linear_addr & 1) ? (uint8_t)(word & 0xff) : (uint8_t)(word >> 8);
    }

    const char *name() override {
        return "mdtang-rtl";
    }

    uint32_t save_base() override {
        return 0x820000; // cart SRAM in SDRAM (mdtang_top's port-3 mapping)
    }

    Vcosim_top top;
};

} // namespace

// Factory used by backend_rtl.cpp. That file (shared unchanged across cores)
// constructs through new_nes_model(); the MDTANG_COSIM_DIR CMake block renames
// that call at compile time for this build (see host/CMakeLists.txt), so one
// binary links exactly one RTL model.
RtlModel *new_md_model();
RtlModel *new_md_model() {
    return new MdModel();
}
