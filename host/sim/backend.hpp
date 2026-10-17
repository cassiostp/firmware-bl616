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
    virtual uint32_t last_video_config() = 0; // last video_config word (0x13)
    // Last clock frame payload (0x14, 13 bytes), empty if none seen. The
    // fake records it; RTL models leave the default.
    virtual std::vector<uint8_t> last_clock_frame() { return {}; }

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

    // Fast ROM loading (GBA): the MCU calls set_baud when it switches the
    // link rate; baud_rate reports the backend's current bit rate (the RTL
    // bridge really serialises/decodes at it, the fake just records it).
    virtual void set_baud(uint32_t baud) = 0;
    virtual uint32_t baud_rate() = 0;
    // Highest set_baud seen (latched): the fastload scripts assert the link
    // really ran at 8M, without racing the brief fast window.
    virtual uint32_t baud_max() = 0;
    // Flip one bit in the next 0x07 payload byte on the wire (fastload CRC
    // test): the core's stream differs from the file, so its 0x0D check
    // must fail and the firmware must retry clean at 2M.
    virtual void corrupt_next_byte() = 0;
    // MODE once rom_bytes() reaches n (fastload MODE-during-load test):
    // deterministic mid-load silence, like trigger_mode from then on.
    virtual void silence_when_rom(uint64_t n, int silence_ms) = 0;

    // Drop the next n core-ID replies (a colliding joypad frame on hardware).
    virtual void drop_ids(int n) = 0;

    // The core's receiver is stuck mid-frame counting down a garbage
    // length (old bitstreams have no RX timeout): it swallows MCU bytes
    // and says nothing until 2048 of them complete the frame, or until it
    // sees a 0x17 link-resync. Fake only.
    virtual void set_rx_stuck(bool on) = 0;

    // Push raw FPGA->MCU bytes, bypassing the backend's framer, so a script
    // can plant a header the firmware parser will stick on.
    virtual void inject_rx(const uint8_t *b, size_t n) = 0;
    // Toggle a pad bit often enough that joy frames keep arriving faster
    // than the firmware's 50 ms mid-frame gap (NES iosys is ~46 ms).
    // raw: also push a joy frame every 40 ms straight into the firmware,
    // so the first one is not waiting on the core's own interval.
    virtual void set_pad_churn(bool on) = 0;
    virtual void set_pad_raw(bool on) { (void)on; }
    // RTL wire faults. The fake ignores them. Each one is applied to the
    // next decoded FPGA->MCU byte(s); a drop/flip/burst is followed by a
    // short raw (unframed) tail so the firmware parser sees the shift.
    virtual void wire_drop(int n) { (void)n; }
    virtual void wire_flip() {}
    virtual void wire_burst(int n) { (void)n; }
    virtual void wire_stall(int ms) { (void)ms; }

    // Continuous game WRAM writes (combo-during-dump tests). The fake has no
    // game model and ignores it; the RTL traffic generator scribbles WRAM.
    virtual void set_churn(bool on) = 0;
    // The save channel answers 0x12 requests (default) or stays mute, like a
    // core whose bitstream lacks it. Fake backend only; the RTL model ignores it.
    virtual void set_save_channel(bool on) = 0;
    // Number of 0x12 save-block requests received (wait-dump watches it).
    virtual uint64_t save_requests() = 0;
    // Number of 0x09 HID frames received (the HID-resend test watches it).
    virtual uint64_t hid_frames() = 0;

    virtual bool overlay_visible() = 0;
    virtual uint64_t rom_bytes() = 0; // ROM payload bytes consumed

    virtual void set_verbose(bool v) = 0;
    virtual std::vector<std::string> log_tail(int n) = 0;
    virtual const char *name() = 0;
};

void fpga_use_fake();
void fpga_select(FpgaBackend *b);
FpgaBackend *fpga_active();
// The RTL (co-simulation) backend: the one core model linked into this
// build, or null when the build has none (see host/CMakeLists.txt).
FpgaBackend *fpga_rtl_backend();
