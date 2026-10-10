// Synchronous fake FPGA core: the other end of the firmware's UART1. Parses
// MCU frames, keeps the OSD text buffer, answers core-ID requests, records
// core_config, and implements the save-RAM channel against an in-memory save
// RAM.
//
// Everything runs in the caller's thread (no background thread, no wall
// clock): tx_push dispatches at once and answers queue instantly in sim
// time, so firmware timeouts only fire when an answer is truly missing.
// All times are sim ticks (see sim_time.hpp).
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "backend.hpp"
#include "fpga.hpp"
#include "sim_time.hpp"

namespace {

class FakeBackend : public FpgaBackend {
  public:
    void run_until(uint64_t) override {}

    void tx_push(uint8_t b) override {
        std::lock_guard<std::recursive_mutex> lk(m_);
        step_frame(b);
    }

    bool rx_available() override {
        std::lock_guard<std::mutex> lk(rx_m_);
        return !rx_.empty();
    }

    uint8_t rx_pop() override {
        std::lock_guard<std::mutex> lk(rx_m_);
        if (rx_.empty())
            return 0;
        uint8_t b = rx_.front();
        rx_.pop_front();
        return b;
    }

    void set_programmed(const char *path) override {
        std::lock_guard<std::recursive_mutex> lk(m_);
        core_file_ = path ? path : "";
        core_id_ = id_for_file(core_file_);
        program_tick_ = sim::now_ticks();
    }

    int programmed_id() override {
        std::lock_guard<std::recursive_mutex> lk(m_);
        return core_id_;
    }

    std::string programmed_file() override {
        std::lock_guard<std::recursive_mutex> lk(m_);
        return core_file_;
    }

    uint64_t ms_since_program() override {
        // No lock needed for the atomic; program_tick_ is only written here
        // but read everywhere, so still take the state lock for ordering.
        std::lock_guard<std::recursive_mutex> lk(m_);
        if (!program_tick_)
            return 0xFFFFFFFFull;
        return (sim::now_ticks() - program_tick_) / sim::TICKS_PER_MS;
    }

    uint32_t last_config() override {
        std::lock_guard<std::recursive_mutex> lk(m_);
        return config_;
    }

    BackendOsd osd_snapshot() override {
        std::lock_guard<std::recursive_mutex> lk(m_);
        BackendOsd s;
        memcpy(s.cells, osd_.cells, sizeof(s.cells));
        s.cursor_col = osd_.cursor_col;
        s.cursor_row = osd_.cursor_row;
        return s;
    }

    uint64_t osd_hash() override {
        std::lock_guard<std::recursive_mutex> lk(m_);
        uint64_t h = 1469598103934665603ull;
        for (int r = 0; r < ROWS; r++)
            for (int c = 0; c < COLS; c++) {
                h ^= (uint64_t)(uint8_t)osd_.cells[r][c];
                h *= 1099511628211ull;
            }
        return h;
    }

    std::vector<int> cursor_rows() override {
        std::lock_guard<std::recursive_mutex> lk(m_);
        std::vector<int> rows;
        for (int r = 0; r < ROWS; r++)
            if (osd_.cells[r][0] == '>')
                rows.push_back(r);
        return rows;
    }

    bool screen_contains(const std::string &s) override {
        if (s.empty())
            return true;
        std::lock_guard<std::recursive_mutex> lk(m_);
        for (int r = 0; r < ROWS; r++) {
            std::string row(osd_.cells[r], COLS);
            if (row.find(s) != std::string::npos)
                return true;
        }
        return false;
    }

    void poke_save(uint16_t off, uint8_t val) override {
        {
            std::lock_guard<std::recursive_mutex> lk(m_);
            if (off < save_ram_.size())
                save_ram_[off] = val;
        }
        uint8_t pad = 0;
        rx_push_frame(0x0B, &pad, 1);
    }

    uint8_t save_ram_byte(uint16_t off) override {
        std::lock_guard<std::recursive_mutex> lk(m_);
        if (off < save_ram_.size())
            return save_ram_[off];
        return 0;
    }

    void wram_write(uint16_t off, uint8_t val) override {
        poke_save(off, val); // the fake has no game path: same observable effect
    }

    void wram_burst(uint16_t off, uint16_t len, uint8_t seed) override {
        {
            std::lock_guard<std::recursive_mutex> lk(m_);
            for (uint32_t i = 0; i < len && off + i < save_ram_.size(); i++)
                save_ram_[off + i] = (uint8_t)(seed + i);
        }
        uint8_t pad = 0; // one dirty notice for the burst, like the DUT's single 0x0B
        rx_push_frame(0x0B, &pad, 1);
    }

    void set_pads(uint16_t p1, uint16_t p2) override {
        std::lock_guard<std::recursive_mutex> lk(m_);
        if (p1 == pad1_ && p2 == pad2_)
            return; // hardware only reports changes
        pad1_ = p1;
        pad2_ = p2;
        uint8_t payload[4] = {(uint8_t)(p1 >> 8), (uint8_t)(p1 & 0xff),
                              (uint8_t)(p2 >> 8), (uint8_t)(p2 & 0xff)};
        rx_push_frame(0x03, payload, sizeof(payload));
    }

    void trigger_mode(int silence_ms) override {
        {
            std::lock_guard<std::recursive_mutex> lk(m_);
            // The FPGA reloads its flash bitstream: core 0 answers again after
            // the silent stretch.
            core_id_ = 0;
            core_file_ = "<flash>";
        }
        silence_until_ = sim::now_ticks() + (uint64_t)silence_ms * sim::TICKS_PER_MS;
    }

    bool overlay_visible() override {
        std::lock_guard<std::recursive_mutex> lk(m_);
        return overlay_visible_;
    }

    uint64_t rom_bytes() override {
        std::lock_guard<std::recursive_mutex> lk(m_);
        return rom_bytes_;
    }

    void set_verbose(bool v) override {
        verbose_.store(v);
    }

    std::vector<std::string> log_tail(int n) override {
        std::lock_guard<std::recursive_mutex> lk(m_);
        if (n <= 0 || n > (int)log_.size())
            n = (int)log_.size();
        return std::vector<std::string>(log_.end() - n, log_.end());
    }

    const char *name() override {
        return "fake";
    }

    void reset_state() {
        std::lock_guard<std::recursive_mutex> lk(m_);
        memset(osd_.cells, ' ', sizeof(osd_.cells));
        osd_.cursor_col = 0;
        osd_.cursor_row = 0;
        if (save_ram_.empty())
            save_ram_.assign(256 * 512, 0xFF);
    }

  private:
    static const int COLS = 32;
    static const int ROWS = 28;
    struct Osd {
        char cells[ROWS][COLS];
        int cursor_col = 0;
        int cursor_row = 0;
    };

    std::recursive_mutex m_;
    std::mutex rx_m_;
    std::deque<uint8_t> rx_;
    Osd osd_;
    int core_id_ = -1;
    std::string core_file_;
    uint64_t program_tick_ = 0;
    uint64_t silence_until_ = 0;
    uint32_t config_ = 0;
    bool overlay_visible_ = true;
    uint64_t rom_bytes_ = 0;
    uint16_t pad1_ = 0, pad2_ = 0;
    std::vector<uint8_t> save_ram_;
    std::atomic<bool> verbose_{false};
    std::vector<std::string> log_;
    std::string log_cur_;

    // Frame parser state (MCU -> fake), advanced one byte per tx_push.
    int pst_ = 0; // 0=AA 1=lenH 2=lenL 3=type 4=payload
    uint16_t plen_ = 0;
    uint8_t ptype_ = 0;
    std::vector<uint8_t> payload_;

    void vlog(const std::string &s) {
        if (verbose_.load())
            fprintf(stderr, "[fpga] %s\n", s.c_str());
        if (log_.size() > 500)
            log_.erase(log_.begin());
        log_.push_back(s);
    }

    void rx_push_frame(uint8_t type, const uint8_t *payload, size_t n) {
        // Frame: AA lenH lenL type payload[len-1]. len counts type+payload.
        uint16_t len = (uint16_t)(n + 1);
        std::lock_guard<std::mutex> lk(rx_m_);
        rx_.push_back(0xAA);
        rx_.push_back((uint8_t)(len >> 8));
        rx_.push_back((uint8_t)(len & 0xff));
        rx_.push_back(type);
        for (size_t i = 0; i < n; i++)
            rx_.push_back(payload[i]);
    }

    bool silenced() {
        return sim::now_ticks() < silence_until_;
    }

    void osd_put(uint8_t b) {
        if (b == '\n') {
            osd_.cursor_col = 0;
            osd_.cursor_row++;
            return;
        }
        if (b == '\r') {
            osd_.cursor_col = 0;
            return;
        }
        if (osd_.cursor_row < ROWS && osd_.cursor_col < COLS)
            osd_.cells[osd_.cursor_row][osd_.cursor_col] = (char)b;
        osd_.cursor_col++;
        if (osd_.cursor_col >= COLS) {
            osd_.cursor_col = 0;
            osd_.cursor_row++;
        }
    }

    void dispatch(uint8_t type, const std::vector<uint8_t> &p) {
        // Caller holds m_.
        switch (type) {
        case 0x01: { // get core ID
            if (silenced() || core_id_ < 0) {
                if (verbose_.load())
                    fprintf(stderr, "[fpga] 0x01 dropped (id=%d)\n", core_id_);
                break;
            }
            uint8_t id = (uint8_t)core_id_;
            rx_push_frame(0x01, &id, 1);
            break;
        }
        case 0x03: // core_config
            if (p.size() >= 4)
                config_ = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                          ((uint32_t)p[2] << 8) | p[3];
            break;
        case 0x04: // overlay cursor
            if (p.size() >= 2) {
                osd_.cursor_col = p[0];
                osd_.cursor_row = p[1];
            }
            break;
        case 0x05: // overlay text
            for (uint8_t b : p)
                osd_put(b);
            break;
        case 0x06: // loading state: nothing to track beyond the log
            break;
        case 0x07: // ROM data
            rom_bytes_ += p.size();
            break;
        case 0x08: // overlay on/off
            if (!p.empty())
                overlay_visible_ = p[0] != 0;
            break;
        case 0x09: // joypad state: nothing to do with it
            break;
        case 0x0b: // PC/XT port write: not emulated
            break;
        case 0x0d: // dprint: keep as log text
            for (uint8_t b : p) {
                if (b == '\n') {
                    vlog("core: " + log_cur_);
                    log_cur_.clear();
                } else {
                    log_cur_ += (char)b;
                }
            }
            break;
        case 0x11: { // save restore: blk16 + 512 bytes
            if (p.size() < 2 + 512)
                break;
            uint16_t blk = ((uint16_t)p[0] << 8) | p[1];
            size_t off = (size_t)blk * 512;
            if (off + 512 <= save_ram_.size())
                memcpy(save_ram_.data() + off, p.data() + 2, 512);
            break;
        }
        case 0x12: { // save request: answer with a 0x0A block
            if (p.size() < 2 || silenced())
                break;
            uint16_t blk = ((uint16_t)p[0] << 8) | p[1];
            size_t off = (size_t)blk * 512;
            static uint8_t blank[512];
            memset(blank, 0xFF, sizeof(blank));
            const uint8_t *data = blank;
            if (off + 512 <= save_ram_.size())
                data = save_ram_.data() + off;
            uint8_t payload[2 + 512];
            payload[0] = (uint8_t)(blk >> 8);
            payload[1] = (uint8_t)(blk & 0xff);
            memcpy(payload + 2, data, 512);
            rx_push_frame(0x0A, payload, sizeof(payload));
            break;
        }
        default:
            break;
        }
    }

    void step_frame(uint8_t b) {
        // Caller holds m_.
        switch (pst_) {
        case 0:
            if (b == 0xAA)
                pst_ = 1;
            break;
        case 1:
            plen_ = (uint16_t)b << 8;
            pst_ = 2;
            break;
        case 2:
            plen_ |= b;
            pst_ = 3;
            break;
        case 3:
            ptype_ = b;
            payload_.clear();
            if (plen_ <= 1) {
                dispatch(ptype_, payload_);
                pst_ = 0;
            } else {
                payload_.reserve(plen_ - 1);
                pst_ = 4;
            }
            break;
        case 4:
            payload_.push_back(b);
            if (payload_.size() >= (size_t)(plen_ - 1)) {
                dispatch(ptype_, payload_);
                pst_ = 0;
            }
            break;
        }
    }

    static int id_for_file(const std::string &path) {
        std::string f = path;
        for (auto &c : f)
            c = (char)tolower(c);
        if (f.find("monitor") != std::string::npos)
            return 0;
        // "snestang" contains "nestang": check it first.
        if (f.find("snestang") != std::string::npos)
            return 2;
        if (f.find("nestang") != std::string::npos)
            return 1;
        if (f.find("gbatang") != std::string::npos)
            return 3;
        if (f.find("mdtang") != std::string::npos)
            return 4;
        if (f.find("smstang") != std::string::npos)
            return 5;
        if (f.find("pctang") != std::string::npos)
            return 6;
        return -1;
    }
};

FakeBackend g_fake;

} // namespace

void set_active_backend(FpgaBackend *b) {
    sim::set_backend(b);
}

namespace {
// The dispatcher: fpga:: calls land here and go to the selected backend.
FpgaBackend *g_active = &g_fake;
} // namespace

void fpga_use_fake() {
    g_active = &g_fake;
    sim::set_backend(&g_fake);
}
FpgaBackend *fpga_active() {
    return g_active;
}
void fpga_select(FpgaBackend *b) {
    g_active = b ? b : (FpgaBackend *)&g_fake;
    sim::set_backend(g_active);
}

namespace fpga {

void start() {
    g_fake.reset_state();
}

void tx_push(uint8_t b) {
    g_active->tx_push(b);
}
bool rx_available() {
    return g_active->rx_available();
}
uint8_t rx_pop() {
    return g_active->rx_pop();
}
void set_programmed(const char *path) {
    g_active->set_programmed(path);
}
int programmed_id() {
    return g_active->programmed_id();
}
std::string programmed_file() {
    return g_active->programmed_file();
}
uint64_t ms_since_program() {
    return g_active->ms_since_program();
}
uint32_t last_config() {
    return g_active->last_config();
}
OsdSnapshot osd_snapshot() {
    BackendOsd s = g_active->osd_snapshot();
    OsdSnapshot o;
    memcpy(o.cells, s.cells, sizeof(o.cells));
    o.cursor_col = s.cursor_col;
    o.cursor_row = s.cursor_row;
    return o;
}
uint64_t osd_hash() {
    return g_active->osd_hash();
}
std::vector<int> cursor_rows() {
    return g_active->cursor_rows();
}
bool screen_contains(const std::string &s) {
    return g_active->screen_contains(s);
}
void poke_save(uint16_t off, uint8_t val) {
    g_active->poke_save(off, val);
}
uint8_t save_ram_byte(uint16_t off) {
    return g_active->save_ram_byte(off);
}
void wram_write(uint16_t off, uint8_t val) {
    g_active->wram_write(off, val);
}
void wram_burst(uint16_t off, uint16_t len, uint8_t seed) {
    g_active->wram_burst(off, len, seed);
}
void set_pads(uint16_t p1, uint16_t p2) {
    g_active->set_pads(p1, p2);
}
void trigger_mode(int silence_ms) {
    g_active->trigger_mode(silence_ms);
}
bool overlay_visible() {
    return g_active->overlay_visible();
}
uint64_t rom_bytes() {
    return g_active->rom_bytes();
}
void set_verbose(bool v) {
    g_active->set_verbose(v);
}
std::vector<std::string> log_tail(int n) {
    return g_active->log_tail(n);
}
const char *backend_name() {
    return g_active->name();
}

} // namespace fpga

#ifndef TANGCORE_HAVE_RTL
// Built without the Verilator model (no docker at build time): only the fake
// core exists. backend_rtl.cpp replaces this stub when RTL is enabled.
FpgaBackend *fpga_rtl_backend() {
    return nullptr;
}
#endif
