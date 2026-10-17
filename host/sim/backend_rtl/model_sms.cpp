// SmsModel: RtlModel for the Master System cosim_top (smstang sim/cosim/).
//
// Core-specific facts encapsulated here (the recipe for the next core lists
// them): the Verilated header names (textdisp and dpram pick Verilator's
// parameter-specialization suffixes), the hierarchical path of the OSD text
// array (iosys sys -> textdisp disp -> DPB menu_mem -> mem, same as NES),
// the SAVE array (there is no SDRAM: smstang's battery RAM is an on-chip
// dpram, port B wired to iosys, so save_base() is 0 and save_byte indexes
// the RAM directly), and the clock ratio: ONE clk per sim tick -- no fclk,
// everything is single-clock; iosys gets FREQ=21_492_000 so its UART stays
// exactly 2 Mbaud in sim time (the real board runs it at 53.7 MHz; only the
// pad throttle's 20 ms stretches, which the firmware's polling hides).
//
// Stepping: two evals per tick (clk low, then clk high -- every edge, per
// the NES recipe's "no rising-edge-only" note). hclk mirrors clk: it only
// feeds textdisp's render pipeline, whose pixels nobody observes.
//
// Factory: new_rtl_model() at the bottom (one core model per build, see
// host/CMakeLists.txt).
#include "rtl_model.hpp"

#include "Vcosim_top.h"
#include "Vcosim_top_cosim_top.h"
#include "Vcosim_top_dpram__Wf.h"
#include "Vcosim_top_gowin_dpb_menu.h"
#include "Vcosim_top_iosys_bl616_cosim__pi1.h"
#include "Vcosim_top_textdisp__Cz1.h"
#include "verilated.h"

namespace {

class SmsModel : public RtlModel {
  public:
    SmsModel() {
        Verilated::debug(0);
        top.clk = 0;
        top.hclk = 0;
        top.resetn = 0;
        top.joy1 = 0;
        top.joy2 = 0;
        top.uart_rx = 1;
        top.silence = 0;
        top.cosim_core_id = 5; // programmed smstang until the bridge says otherwise
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
            top.hclk = 0;
            top.eval();
            top.clk = 1;
            top.hclk = 1;
            top.eval();
            if (tx_levels)
                tx_levels[i] = top.uart_tx ? 1 : 0;
        }
        pins.uart_tx = top.uart_tx != 0;
        pins.tx_pending = top.tx_pending != 0;
        pins.core_config = top.core_config;
        pins.video_config = top.video_config;
        pins.overlay = top.overlay != 0;
        pins.busy = false; // no SDRAM controller to initialize (and unused)
        pins.rom_bytes = top.rom_bytes;
        pins.poke_ack = top.poke_ack != 0;
    }

    uint8_t osd_byte(int row, int col) override {
        // DPB address is {1'b0, y[4:0], x[4:0]} (see textdisp.v).
        return top.cosim_top->sys->disp->menu_mem->mem[row * 32 + col];
    }

    uint8_t save_byte(uint32_t linear_addr) override {
        // The nvram is the 32 KB dpram itself: linear == byte address.
        return top.cosim_top->nvram->mem[linear_addr & 0x7fff];
    }

    const char *name() override {
        return "smstang-rtl";
    }

    uint32_t save_base() override {
        return 0; // on-chip battery RAM, no window mapping
    }

    Vcosim_top top;
};

} // namespace

RtlModel *new_rtl_model();
RtlModel *new_rtl_model() {
    return new SmsModel();
}
