// RTL FPGA backend: the real core interface logic (Verilated cosim_top)
// as the other end of the firmware's UART1. Shared across cores: it speaks
// only the RtlModel interface (rtl_model.hpp), so per-core differences
// (save window, array paths, clock ratio) live in model_<core>.cpp.
//
// UART (bit-level at the real 2 Mbaud, FREQ=21.492MHz -> 10.746 ticks/bit):
//   MCU -> FPGA bytes queue in tx_queue (any thread) and serialize from
//   there, chained back-to-back like the MCU's own UART (1 start + 8 data +
//   1 stop bit; the receiver needs >= 1 stop). No arrival stamps: order is
//   the queue order and the first byte starts at the next step position
//   (<= 1 batch late, far inside the firmware's 50 ms resync margin).
//   FPGA -> MCU bits are sampled every tick and decoded with per-byte start
//   resync (2 stop bits on the wire); complete frames land in rx_queue byte
//   for byte, exactly like a UART FIFO. No byte on the wire is rewritten:
//   the core-ID reply carries the programmed ID because programming drives
//   the model's cosim_core_id input (see nestang sim/cosim/).
// TIME (see sim_time.hpp): run_until() steps the model in <= 256-tick
// pieces (split at MCU waveform edges so uart_rx is constant per piece).
// Fully quiet pieces (nothing scheduled, decoder idle, TX line idle-high,
// no TX reply owed by the model, no poke pending, reset released) JUMP
// without evaluating: only dead air is skipped, so bus contention during
// real traffic (save dumps vs game accesses) is fully modeled. The
// TX-pending gate matters: after a request is consumed the line is idle but
// the reply has not started yet, and jumping then would skip it unsampled.
// THREADS: the model object is touched only by run_until (always under the
// sim scheduler lock: single stepper). Queues/records have their own small
// mutexes, never held across stepping. Cross-thread inputs (pads, poke
// requests, silence, reset, programmed ID) are atomics applied per batch;
// frequently-read outputs are cached in atomics per batch. OSD/save-RAM
// array reads go straight at the public arrays (documented benign race:
// worst case a torn snapshot delays one script poll, like sampling real
// hardware mid-frame).
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "backend.hpp"
#include "rtl_model.hpp"

RtlModel *new_nes_model(); // model_nes.cpp

namespace {

const double TICKS_PER_BIT = 21492000.0 / 2000000.0; // 10.746
const uint64_t BYTE_TICKS = 107;                     // 10 bits at 2 Mbaud
const int BATCH = 256;                               // max ticks per step piece
const int QUIET_HIGH_TICKS = 16;                     // TX idle-high run to jump
const size_t TX_LIMIT = 4096; // backlog bound: biggest legit burst is a 1 KB
                              // ROM frame; past this putchar blocks (UART FIFO)
const uint64_t RESET_TICKS = 30;                     // resetn stretch

uint64_t bit_at(uint64_t start, int i) {
    // Mid-bit sample tick for data bit i (start = falling edge tick).
    return start + (uint64_t)llround((1.5 + i) * TICKS_PER_BIT);
}

int id_for_file(const std::string &path) {
    // Bitstream filename -> core ID, same map as the fake backend (and the
    // firmware's core_info): the model answers as the programmed core.
    std::string f = path;
    for (auto &c : f)
        c = (char)tolower(c);
    if (f.find("monitor") != std::string::npos)
        return 0;
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

class RtlBackend : public FpgaBackend {
  public:
    explicit RtlBackend(RtlModel *m) : model(m) {
        reset_until.store(RESET_TICKS, std::memory_order_relaxed); // power-on stretch
    }

    // ---- sim::Backend ----
    void run_until(uint64_t target) override {
        uint64_t now = pos.load(std::memory_order_relaxed);
        while (now < target) {
            uint64_t batch_end = std::min(target, now + BATCH);
            step_batch(batch_end);
            now = pos.load(std::memory_order_relaxed);
        }
    }

    // ---- FpgaBackend ----
    void tx_push(uint8_t b) override {
        std::unique_lock<std::mutex> lk(tx_m);
        tx_queue.push_back(b);
        backlog.fetch_add(1, std::memory_order_relaxed);
        // Backpressure (hardware UART FIFO behavior): the firmware can send
        // faster than the model steps bytes (107 ticks each), e.g. the menu
        // redraws every input-loop pass with no idle delay. Without a bound
        // the backlog grows without limit while sim time crawls, burying
        // later frames (pad presses) and eating gigabytes. Block like a full
        // FIFO, pumping sim time while blocked so the block always drains
        // (no deadlock even if this thread is the only driver).
        while (backlog.load(std::memory_order_relaxed) > TX_LIMIT) {
            lk.unlock();
            // Advance a little: steps the model through the backlog. No
            // sim:: lock is held here (tx_m is dropped), so this nests
            // safely with run_until on other threads.
            sim::advance_to(sim::now_ticks() + 5 * sim::TICKS_PER_MS);
            lk.lock();
        }
        // Sniff the MCU -> FPGA stream for 0x12 save requests (wait-dump).
        switch (tx_ps) {
        case 0:
            tx_ps = (b == 0xAA) ? 1 : 0;
            break;
        case 1:
            tx_plen = (uint16_t)b << 8;
            tx_ps = 2;
            break;
        case 2:
            tx_plen |= b;
            tx_ps = 3;
            break;
        case 3:
            if (b == 0x12)
                save_reqs.fetch_add(1, std::memory_order_relaxed);
            tx_type = b;
            tx_ps = (tx_plen <= 1) ? 0 : 4;
            tx_left = tx_plen > 1 ? tx_plen - 1 : 0;
            if (tx_ps == 0 && verbose.load(std::memory_order_relaxed)) {
                char msg[64];
                snprintf(msg, sizeof(msg), "tx frame type=0x%02x len=%u", tx_type, tx_plen);
                vlog(msg);
            }
            break;
        case 4:
            if (--tx_left == 0) {
                tx_ps = 0;
                if (verbose.load(std::memory_order_relaxed)) {
                    char msg[64];
                    snprintf(msg, sizeof(msg), "tx frame type=0x%02x len=%u", tx_type,
                             tx_plen);
                    vlog(msg);
                }
            }
            break;
        }
    }
    // tx_push parser state (under tx_m) + 0x12 counter.
    int tx_ps = 0;
    uint16_t tx_plen = 0, tx_left = 0;
    uint8_t tx_type = 0;
    std::atomic<uint64_t> save_reqs{0};

    bool rx_available() override {
        std::lock_guard<std::mutex> lk(rx_m);
        return !rx_queue.empty();
    }
    uint8_t rx_pop() override {
        std::lock_guard<std::mutex> lk(rx_m);
        if (rx_queue.empty())
            return 0;
        uint8_t b = rx_queue.front();
        rx_queue.pop_front();
        return b;
    }
    void set_programmed(const char *path) override {
        std::string f = path ? path : "";
        int id = id_for_file(f);
        {
            std::lock_guard<std::mutex> lk(rec_m);
            core_file = f;
            if (id >= 0)
                core_id_rec = id;
        }
        if (id >= 0)
            core_id.store((uint16_t)id, std::memory_order_relaxed);
        else
            vlog("programmed " + f + ": unknown core, keeping ID");
        program_tick = sim::now_ticks();
        program_tick_set = true;
        reset_until.store(pos.load(std::memory_order_relaxed) + RESET_TICKS,
                          std::memory_order_relaxed);
        drop_pending.store(true, std::memory_order_relaxed);
        vlog("programmed " + f + " as core " + std::to_string(id));
    }
    int programmed_id() override {
        std::lock_guard<std::mutex> lk(rec_m);
        return core_id_rec;
    }
    std::string programmed_file() override {
        std::lock_guard<std::mutex> lk(rec_m);
        return core_file;
    }
    uint64_t ms_since_program() override {
        if (!program_tick_set)
            return 0xFFFFFFFFull;
        // program_tick is written by the script thread, read here and there;
        // benign race (ms precision, monotonic clock).
        uint64_t now = sim::now_ticks();
        return now >= program_tick ? (now - program_tick) / sim::TICKS_PER_MS : 0;
    }
    uint32_t last_config() override {
        return last_config_cached.load(std::memory_order_relaxed);
    }
    BackendOsd osd_snapshot() override {
        BackendOsd s;
        for (int r = 0; r < BackendOsd::ROWS; r++)
            for (int c = 0; c < BackendOsd::COLS; c++)
                s.cells[r][c] = (char)model->osd_byte(r, c);
        s.cursor_col = 0;
        s.cursor_row = 0;
        return s;
    }
    uint64_t osd_hash() override {
        uint64_t h = 1469598103934665603ull;
        for (int r = 0; r < BackendOsd::ROWS; r++)
            for (int c = 0; c < BackendOsd::COLS; c++) {
                h ^= (uint64_t)(uint8_t)model->osd_byte(r, c);
                h *= 1099511628211ull;
            }
        return h;
    }
    std::vector<int> cursor_rows() override {
        std::vector<int> rows;
        for (int r = 0; r < BackendOsd::ROWS; r++)
            if (model->osd_byte(r, 0) == '>')
                rows.push_back(r);
        return rows;
    }
    bool screen_contains(const std::string &s) override {
        if (s.empty())
            return true;
        for (int r = 0; r < BackendOsd::ROWS; r++) {
            char row[BackendOsd::COLS + 1];
            for (int c = 0; c < BackendOsd::COLS; c++)
                row[c] = (char)model->osd_byte(r, c);
            row[BackendOsd::COLS] = 0;
            if (strstr(row, s.c_str()))
                return true;
        }
        return false;
    }
    void poke_save(uint16_t off, uint8_t val) override {
        // Like the fake's poke (write + dirty notice), but through the real
        // game path: a CPU-port WRAM write, so sv_core_we dirties the save
        // exactly as a running game would. Lands within a few chunks (the
        // model must be stepped to sample it); scripts poll for the effect.
        queue_poke(off, val);
    }
    uint8_t save_ram_byte(uint16_t off) override {
        return model->save_byte(model->save_base() + off);
    }
    void wram_write(uint16_t off, uint8_t val) override {
        queue_poke(off, val);
    }
    void wram_burst(uint16_t off, uint16_t len, uint8_t seed) override {
        std::lock_guard<std::mutex> lk(poke_m);
        for (uint32_t i = 0; i < len; i++)
            poke_queue.push_back({(uint16_t)(off + i), (uint8_t)(seed + i)});
    }
    void set_pads(uint16_t p1, uint16_t p2) override {
        // Change-detect is the core's own (20 ms throttle, like hardware).
        joy1.store(p1, std::memory_order_relaxed);
        joy2.store(p2, std::memory_order_relaxed);
    }
    void set_churn(bool on) override {
        churn.store(on, std::memory_order_relaxed);
    }

    uint64_t save_requests() override {
        return save_reqs.load(std::memory_order_relaxed);
    }

    void trigger_mode(int silence_ms) override {
        silence_active.store(true, std::memory_order_relaxed);
        silence_until.store(sim::now_ticks() + (uint64_t)silence_ms * sim::TICKS_PER_MS,
                            std::memory_order_relaxed);
        drop_pending.store(true, std::memory_order_relaxed); // in-flight dies
        vlog("MODE: silent for " + std::to_string(silence_ms) + " ms");
    }
    bool overlay_visible() override {
        return overlay_cached.load(std::memory_order_relaxed);
    }
    uint64_t rom_bytes() override {
        return rom_bytes_cached.load(std::memory_order_relaxed);
    }
    void set_verbose(bool v) override {
        verbose.store(v, std::memory_order_relaxed);
    }
    std::vector<std::string> log_tail(int n) override {
        std::lock_guard<std::mutex> lk(log_m);
        if (n <= 0 || n > (int)log.size())
            n = (int)log.size();
        return std::vector<std::string>(log.end() - n, log.end());
    }
    const char *name() override {
        return model->name();
    }

  private:
    RtlModel *model;
    RtlPins pins;
    std::atomic<uint64_t> pos{0}; // model time == sim time (jumps skip dead air)

    // MCU -> FPGA serial schedule: {start tick, byte} chained back-to-back.
    // Touched only by run_until (single stepper); tx_queue feeds it.
    std::mutex tx_m;
    std::deque<uint8_t> tx_queue; // pushed by firmware threads
    std::atomic<size_t> backlog{0}; // tx_queue + sched entries (unsent bytes)
    struct SchedByte {
        uint64_t start;
        uint8_t b;
    };
    std::deque<SchedByte> sched;
    uint64_t line_free = 0;

    // FPGA -> MCU decode + frame parser state (run_until only).
    std::mutex rx_m;
    std::deque<uint8_t> rx_queue;
    bool dec_idle = true;
    uint64_t dec_start = 0;
    int dec_bit = 0;
    uint8_t dec_byte = 0;
    int tx_high_streak = QUIET_HIGH_TICKS;
    int fpos = 0;
    uint16_t flen = 0;
    uint8_t ftype = 0;
    std::vector<uint8_t> frame_pending;

    // Cross-thread inputs / cached outputs.
    std::atomic<uint16_t> joy1{0}, joy2{0};
    std::atomic<bool> churn{false};
    std::atomic<uint16_t> core_id{1};
    std::atomic<uint32_t> last_config_cached{0};
    std::atomic<bool> overlay_cached{true};
    std::atomic<uint32_t> rom_bytes_cached{0};

    // Records + log.
    std::mutex rec_m;
    std::string core_file;
    int core_id_rec = -1;
    uint64_t program_tick = 0;
    bool program_tick_set = false;
    std::mutex log_m;
    std::vector<std::string> log;
    std::atomic<bool> verbose{false};

    // Poke queue (game-path WRAM writes) + drop flag for resets. The drop
    // runs inside run_until (single stepper); setters only raise the flag.
    struct Poke {
        uint16_t off;
        uint8_t val;
    };
    std::mutex poke_m;
    std::deque<Poke> poke_queue;
    bool poke_arming = false; // asserted, waiting a batch for ack
    bool poke_asserted = false;
    std::atomic<bool> drop_pending{false};

    // Silence / reset bookkeeping (ticks, applied per batch).
    std::atomic<bool> silence_active{false};
    std::atomic<uint64_t> silence_until{0};
    std::atomic<uint64_t> reset_until{0};

    void vlog(const std::string &s) {
        if (verbose.load(std::memory_order_relaxed))
            fprintf(stderr, "[rtl] %s\n", s.c_str());
        std::lock_guard<std::mutex> lk(log_m);
        if (log.size() > 500)
            log.erase(log.begin());
        log.push_back(s);
    }

    void step_batch(uint64_t batch_end) {
        uint64_t now = pos.load(std::memory_order_relaxed);
        // Latch cross-thread inputs.
        pins.joy1 = joy1.load(std::memory_order_relaxed);
        pins.joy2 = joy2.load(std::memory_order_relaxed);
        pins.churn = churn.load(std::memory_order_relaxed);
        pins.core_id = core_id.load(std::memory_order_relaxed);
        if (drop_pending.load(std::memory_order_relaxed)) {
            drop_pending.store(false, std::memory_order_relaxed);
            drop_traffic(now);
        }
        // Silence / reset accounting.
        if (silence_active.load(std::memory_order_relaxed) &&
            now >= silence_until.load(std::memory_order_relaxed)) {
            silence_active.store(false, std::memory_order_relaxed);
            core_id.store(0, std::memory_order_relaxed); // flash answers again
            pins.core_id = 0;
            reset_until.store(now + RESET_TICKS, std::memory_order_relaxed);
            drop_traffic(now);
            vlog("silence over: reset, core 0");
        }
        pins.silence = silence_active.load(std::memory_order_relaxed);
        pins.resetn = now >= reset_until.load(std::memory_order_relaxed);
        pump_poke();
        // Drain newly pushed MCU bytes into the serial schedule.
        {
            std::lock_guard<std::mutex> lk(tx_m);
            while (!tx_queue.empty()) {
                uint64_t start = line_free > now ? line_free : now;
                sched.push_back({start, tx_queue.front()});
                tx_queue.pop_front();
                line_free = start + BYTE_TICKS;
            }
        }
        if (pins.silence) {
            // Reloading FPGA: the model ignores the line; track the waveform
            // arithmetically (no evals) and skip stepping.
            pos.store(batch_end, std::memory_order_relaxed);
            tx_high_streak = QUIET_HIGH_TICKS;
            return;
        }
        if (sched.empty() && tx_high_streak >= QUIET_HIGH_TICKS && !poke_pending() &&
            dec_idle && fpos == 0 && !pins.tx_pending &&
            now >= reset_until.load(std::memory_order_relaxed)) {
            pos.store(batch_end, std::memory_order_relaxed); // dead air: jump
            tx_high_streak = 0; // re-qualify after landing (line unsampled mid-jump)
            return;
        }
        // Step in pieces split at MCU waveform edges (uart_rx constant
        // within a piece); sample TX every tick for the decoder.
        uint8_t txbuf[BATCH];
        uint64_t p = now;
        while (p < batch_end) {
            uint64_t edge = next_edge(p);
            uint64_t piece = std::min(batch_end, edge);
            if (piece <= p)
                piece = p + 1;
            pins.uart_rx = level_at(p);
            model->step(piece - p, pins, txbuf);
            // NOTE: capture the count first: `p` advances in this loop, so
            // `i < piece - p` would terminate at half the ticks and overlap
            // every piece (double-stepping the model breaks its UART).
            uint64_t nstep = piece - p;
            for (uint64_t i = 0; i < nstep; i++, p++)
                sample_tx(txbuf[i], p);
            // Refresh slow outputs per piece (ack latency ~ one piece).
            last_config_cached.store(pins.core_config, std::memory_order_relaxed);
            overlay_cached.store(pins.overlay, std::memory_order_relaxed);
            rom_bytes_cached.store(pins.rom_bytes, std::memory_order_relaxed);
        }
        pos.store(batch_end, std::memory_order_relaxed);
    }

    int level_at(uint64_t t) {
        // MCU waveform level at tick t (prunes consumed bytes).
        while (!sched.empty() && t >= sched.front().start + BYTE_TICKS) {
            sched.pop_front();
            backlog.fetch_sub(1, std::memory_order_relaxed);
        }
        if (sched.empty() || sched.front().start > t)
            return 1;
        uint64_t dt = t - sched.front().start;
        uint8_t b = sched.front().b;
        if (dt < (uint64_t)llround(1 * TICKS_PER_BIT))
            return 0; // start bit
        for (int i = 0; i < 8; i++) {
            uint64_t edge = (uint64_t)llround((1 + i) * TICKS_PER_BIT);
            uint64_t next = (uint64_t)llround((2 + i) * TICKS_PER_BIT);
            if (dt >= edge && dt < next)
                return (b >> i) & 1;
        }
        return 1; // stop bit
    }

    uint64_t next_edge(uint64_t from) {
        // Next scheduled waveform change at/after `from` (front entry only;
        // entries are chained in order and level_at prunes the consumed).
        if (sched.empty())
            return UINT64_MAX;
        if (sched.front().start >= from)
            return sched.front().start;
        for (int i = 0; i <= 9; i++) {
            uint64_t e = sched.front().start + (uint64_t)llround(i * TICKS_PER_BIT);
            if (e >= from)
                return e;
        }
        // Current byte done; next byte (if any) starts at/after its start.
        if (sched.size() > 1)
            return sched[1].start;
        return UINT64_MAX;
    }

    void sample_tx(int level, uint64_t tick) {
        if (level)
            tx_high_streak++;
        else
            tx_high_streak = 0;
        if (dec_idle) {
            if (!level) { // falling edge: start bit
                dec_idle = false;
                dec_start = tick;
                dec_bit = 0;
                dec_byte = 0;
            }
            return;
        }
        while (dec_bit < 8 && tick >= bit_at(dec_start, dec_bit)) {
            if (level)
                dec_byte |= (uint8_t)(1 << dec_bit);
            dec_bit++;
        }
        if (dec_bit == 8 && tick >= dec_start + (uint64_t)llround(9.5 * TICKS_PER_BIT)) {
            frame_byte(dec_byte);
            dec_idle = true;
        }
    }

    void frame_byte(uint8_t b) {
        if (fpos == 0) {
            if (b == 0xAA)
                fpos = 1;
            return;
        } else if (fpos == 1) {
            flen = (uint16_t)b << 8;
            fpos = 2;
            return;
        } else if (fpos == 2) {
            flen |= b;
            fpos = 3;
            return;
        } else if (fpos == 3) {
            ftype = b;
            fpos = 4;
            frame_pending.clear();
            if (flen <= 1)
                frame_done();
            return;
        }
        frame_pending.push_back(b);
        if ((int)frame_pending.size() >= (int)flen - 1)
            frame_done();
    }

    void frame_done() {
        std::lock_guard<std::mutex> lk(rx_m);
        rx_queue.push_back(0xAA);
        rx_queue.push_back((uint8_t)(flen >> 8));
        rx_queue.push_back((uint8_t)(flen & 0xff));
        rx_queue.push_back(ftype);
        for (uint8_t b : frame_pending)
            rx_queue.push_back(b);
        if (verbose.load(std::memory_order_relaxed)) {
            char msg[64];
            snprintf(msg, sizeof(msg), "rx frame type=0x%02x len=%u", ftype, flen);
            // NOTE: vlog takes log_m; called with rx_m held -- both are leaf
            // locks, never held across stepping or each other elsewhere, so
            // no cycle. (Verbose path only.)
            vlog(msg);
        }
        fpos = 0;
    }

    void drop_traffic(uint64_t now) {
        // Reprogramming/reload loses in-flight bytes (both directions'
        // partial frames); complete bytes already queued for the firmware
        // (its UART FIFO equivalent) survive.
        {
            std::lock_guard<std::mutex> lk(tx_m);
            backlog.fetch_sub(tx_queue.size(), std::memory_order_relaxed);
            tx_queue.clear();
        }
        backlog.fetch_sub(sched.size(), std::memory_order_relaxed);
        sched.clear();
        line_free = now;
        dec_idle = true;
        fpos = 0;
        frame_pending.clear();
        {
            std::lock_guard<std::mutex> lk(poke_m);
            poke_queue.clear();
        }
        poke_arming = false;
        poke_asserted = false;
        pins.poke_valid = false;
    }

    bool poke_pending() {
        std::lock_guard<std::mutex> lk(poke_m);
        return !poke_queue.empty() || poke_arming || poke_asserted;
    }

    void queue_poke(uint16_t off, uint8_t val) {
        std::lock_guard<std::mutex> lk(poke_m);
        poke_queue.push_back({off, val});
    }

    void pump_poke() {
        // Two-phase across batches (no inline stepping): assert for a batch,
        // then check ack and release.
        if (poke_asserted) {
            pins.poke_valid = false;
            poke_asserted = false;
            poke_arming = false;
            return;
        }
        if (poke_arming) {
            if (pins.poke_ack) {
                std::lock_guard<std::mutex> lk(poke_m);
                if (!poke_queue.empty())
                    poke_queue.pop_front();
                poke_asserted = true; // released next batch
            }
            // (no ack yet: keep asserting; a later batch retries)
            return;
        }
        std::lock_guard<std::mutex> lk(poke_m);
        if (!poke_queue.empty()) {
            pins.poke_valid = true;
            pins.poke_off = poke_queue.front().off;
            pins.poke_data = poke_queue.front().val;
            poke_arming = true;
        } else {
            pins.poke_valid = false;
        }
    }
};

RtlBackend *g_rtl = nullptr;

} // namespace

FpgaBackend *fpga_rtl_backend() {
    if (!g_rtl)
        g_rtl = new RtlBackend(new_nes_model());
    return g_rtl;
}
