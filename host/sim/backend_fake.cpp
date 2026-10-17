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
#include <cstdint>
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
    void run_until(uint64_t target) override {
        if (!pad_churn_)
            return;
        // Joy frames every 40 ms: under the firmware's 50 ms gap, so a
        // parser stuck inside a long frame never self-heals on silence.
        uint64_t now = sim::now_ticks();
        if (pad_next_ == 0)
            pad_next_ = now;
        while (pad_next_ <= target) {
            uint16_t p1;
            bool stuck;
            {
                std::lock_guard<std::recursive_mutex> lk(m_);
                pad1_ ^= 0x080; // right: not part of the menu combo
                p1 = pad1_;
                stuck = rx_stuck_;
            }
            if (!stuck) {
                uint8_t payload[4] = {(uint8_t)(p1 >> 8), (uint8_t)(p1 & 0xff), 0, 0};
                rx_push_frame(0x03, payload, 4);
            }
            pad_next_ += 40ull * sim::TICKS_PER_MS;
        }
    }

    void tx_push(uint8_t b) override {
        std::lock_guard<std::recursive_mutex> lk(m_);
        if (rx_stuck_) {
            // Counting down a garbage frame length: the byte is a
            // parameter now. 2048 zeros (the old flush) complete any frame
            // iosys accepts. A 0x17 link-resync does it in one command.
            stuck_hist_ = (stuck_hist_ << 8) | b;
            if (stuck_hist_ == 0xAA000117u) {
                rx_stuck_ = false;
                pst_ = 0;
                rx_stuck_bytes_ = 0;
                if (verbose_.load())
                    fprintf(stderr, "[fpga] rx unstuck on 0x17\n");
                return;
            }
            if (++rx_stuck_bytes_ >= 2048) {
                rx_stuck_ = false;
                pst_ = 0;
                if (verbose_.load())
                    fprintf(stderr, "[fpga] rx unstuck after 2048 bytes\n");
            }
            return;
        }
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
        // A freshly programmed core is silent while it starts. The firmware
        // must not mistake that for MODE (controls_reset/seen_answer).
        silence_until_ = sim::now_ticks() + (uint64_t)250 * sim::TICKS_PER_MS;
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

    uint32_t last_video_config() override {
        std::lock_guard<std::recursive_mutex> lk(m_);
        return video_config_;
    }

    std::vector<uint8_t> last_clock_frame() override {
        std::lock_guard<std::recursive_mutex> lk(m_);
        return clock_frame_;
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
        if (rx_stuck_)
            return; // the stalled core says nothing at all (QA4 shape)
        uint8_t payload[4] = {(uint8_t)(p1 >> 8), (uint8_t)(p1 & 0xff),
                              (uint8_t)(p2 >> 8), (uint8_t)(p2 & 0xff)};
        rx_push_frame(0x03, payload, sizeof(payload));
    }

    void set_churn(bool) override {} // no game model on the fake
    void set_save_channel(bool on) override { save_channel_ = on; }
    void set_rx_stuck(bool on) override {
        std::lock_guard<std::recursive_mutex> lk(m_);
        rx_stuck_ = on;
        rx_stuck_bytes_ = 0;
        stuck_hist_ = 0;
    }

    void inject_rx(const uint8_t *b, size_t n) override {
        if (!b || !n)
            return;
        {
            std::lock_guard<std::mutex> lk(rx_m_);
            for (size_t i = 0; i < n; i++)
                rx_.push_back(b[i]);
        }
        fpga::rx_notify();
    }

    void set_pad_churn(bool on) override {
        pad_churn_ = on;
        if (on)
            pad_next_ = 0; // first frame on the next step
    }
    void set_pad_raw(bool on) override {
        set_pad_churn(on); // the fake's churn is already a 40 ms raw frame
    }

    uint64_t save_requests() override {
        return save_reqs_.load(std::memory_order_relaxed);
    }

    uint64_t blank_lines() override {
        return blank_lines_.load(std::memory_order_relaxed);
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

    // Fast ROM loading: the fake never answers 0x15/0x16 (no FAST_UART), so
    // the firmware always falls back to 2M here -- that fallback is what the
    // fake-suite GBA tests prove. Baud is recorded for expect-baud.
    void set_baud(uint32_t baud) override {
        std::lock_guard<std::recursive_mutex> lk(m_);
        baud_ = baud;
        if (baud > baud_max_)
            baud_max_ = baud;
    }
    uint32_t baud_rate() override {
        std::lock_guard<std::recursive_mutex> lk(m_);
        return baud_;
    }
    uint32_t baud_max() override {
        std::lock_guard<std::recursive_mutex> lk(m_);
        return baud_max_;
    }
    void corrupt_next_byte() override {} // no wire model: nothing to flip
    void silence_when_rom(uint64_t n, int silence_ms) override {
        std::lock_guard<std::recursive_mutex> lk(m_);
        silence_rom_at_ = n;
        silence_rom_ms_ = silence_ms;
    }

    void drop_ids(int n) override {
        std::lock_guard<std::recursive_mutex> lk(m_);
        if (n > 0)
            drop_ids_ += n;
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
            save_ram_.assign(512 * 512, 0xFF);  // incl. a GBA RTC extra sector
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
    uint32_t baud_ = 2000000;   // last set_baud (the fake never switches)
    uint32_t baud_max_ = 2000000;
    uint64_t silence_rom_at_ = UINT64_MAX;  // silence_when_rom threshold
    int silence_rom_ms_ = 0;
    std::string core_file_;
    uint64_t program_tick_ = 0;
    uint64_t silence_until_ = 0;
    int drop_ids_ = 0;
    uint32_t config_ = 0;
    uint32_t video_config_ = 0;
    std::vector<uint8_t> clock_frame_;
    bool overlay_visible_ = true;
    uint64_t rom_bytes_ = 0;
    uint16_t pad1_ = 0, pad2_ = 0;
    std::vector<uint8_t> save_ram_;
    bool save_channel_ = true;
    bool rx_stuck_ = false;
    uint32_t rx_stuck_bytes_ = 0;
    uint32_t stuck_hist_ = 0; // last four MCU bytes, for the 0x17 unstick
    bool pad_churn_ = false;
    uint64_t pad_next_ = 0;
    std::atomic<uint64_t> save_reqs_{0};
    std::atomic<uint64_t> blank_lines_{0};
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
        {
            std::lock_guard<std::mutex> lk(rx_m_);
            rx_.push_back(0xAA);
            rx_.push_back((uint8_t)(len >> 8));
            rx_.push_back((uint8_t)(len & 0xff));
            rx_.push_back(type);
            for (size_t i = 0; i < n; i++)
                rx_.push_back(payload[i]);
        }
        fpga::rx_notify();
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
            if (drop_ids_ > 0) {
                drop_ids_--;
                if (verbose_.load())
                    fprintf(stderr, "[fpga] 0x01 dropped (injected)\n");
                break;
            }
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
        case 0x13: // video_config
            if (p.size() >= 4)
                video_config_ = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                                ((uint32_t)p[2] << 8) | p[3];
            break;
        case 0x14: // clock frame: record for expect-clock
            clock_frame_ = p;
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
            if (p.size() == 32) {
                // overlay_clear() blanks a row with 32 spaces; nothing
                // else prints a full-blank line (ui/overlay.cpp).
                bool blank = true;
                for (uint8_t b : p)
                    if (b != ' ')
                        blank = false;
                if (blank)
                    blank_lines_.fetch_add(1, std::memory_order_relaxed);
            }
            break;
        case 0x06: // loading state: nothing to track beyond the log
            break;
        case 0x07: // ROM data
            rom_bytes_ += p.size();
            if (silence_rom_at_ != UINT64_MAX && rom_bytes_ >= silence_rom_at_) {
                silence_rom_at_ = UINT64_MAX;   // fire once, like a MODE press
                core_id_ = 0;
                core_file_ = "<flash>";
                silence_until_ = sim::now_ticks() + (uint64_t)silence_rom_ms_ * sim::TICKS_PER_MS;
            }
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
            save_reqs_.fetch_add(1, std::memory_order_relaxed);
            if (p.size() < 2 || silenced() || !save_channel_)
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

namespace {
void (*g_rx_hook)() = nullptr;
} // namespace

namespace fpga {

void start() {
    g_fake.reset_state();
}

void tx_push(uint8_t b) {
    g_active->tx_push(b);
}
void set_rx_hook(void (*fn)()) {
    g_rx_hook = fn;
}
void rx_notify() {
    if (g_rx_hook)
        g_rx_hook();
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
uint32_t last_video_config() {
    return g_active->last_video_config();
}
std::vector<uint8_t> last_clock_frame() {
    return g_active->last_clock_frame();
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
void set_baud(uint32_t baud) {
    g_active->set_baud(baud);
}
uint32_t baud_rate() {
    return g_active->baud_rate();
}
uint32_t baud_max() {
    return g_active->baud_max();
}
void corrupt_next_byte() {
    g_active->corrupt_next_byte();
}
void silence_when_rom(uint64_t n, int silence_ms) {
    g_active->silence_when_rom(n, silence_ms);
}
void drop_ids(int n) {
    g_active->drop_ids(n);
}
void set_rx_stuck(bool on) {
    g_active->set_rx_stuck(on);
}
void inject_rx(const uint8_t *b, size_t n) {
    g_active->inject_rx(b, n);
}
void set_pad_churn(bool on) {
    g_active->set_pad_churn(on);
}
void set_pad_raw(bool on) {
    g_active->set_pad_raw(on);
}
void wire_drop(int n) {
    g_active->wire_drop(n);
}
void wire_flip() {
    g_active->wire_flip();
}
void wire_burst(int n) {
    g_active->wire_burst(n);
}
void wire_stall(int ms) {
    g_active->wire_stall(ms);
}
void set_churn(bool on) {
    g_active->set_churn(on);
}
void set_save_channel(bool on) {
    g_active->set_save_channel(on);
}
uint64_t save_requests() {
    return g_active->save_requests();
}
uint64_t blank_lines() {
    return g_active->blank_lines();
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
