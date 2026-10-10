// tangcore-sim: Linux host simulator for the TangCore BL616 firmware.
//
// Runs the REAL firmware (main.cpp, ui/, utils/, core/) against a fake FPGA
// core and a host-directory "SD card". Two modes:
//
//   interactive:  tangcore-sim --sd <dir>
//   scripted:     tangcore-sim --sd <dir> --script <file> [--script-pos N]
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "ff.h"
#include "utils.h"
#include "overlay.h"

#include "fpga.hpp"
#include "host.h"
#include "backend.hpp"
#include "sim_time.hpp"

namespace sim_rtos {
void init_timebase();
uint64_t uptime_ms();
} // namespace sim_rtos

std::atomic<bool> g_reset_requested{false};

void sim_request_reset() {
    g_reset_requested.store(true);
}

uint64_t sim_uptime_ms() {
    return sim_rtos::uptime_ms();
}

namespace {

std::string g_argv0;
std::string g_sdroot;
std::string g_script;
std::string g_core = "fake";
long g_script_pos = 0;
bool g_verbose = false;

std::atomic<int> g_current_line{0};

// Sim time (see sim_time.hpp): everything the firmware and the script do is
// measured in it. Advancing yields wall-clock time between 5 ms chunks so the
// firmware threads keep running while the script waits.
uint64_t now_ms() {
    return sim::now_ms();
}

void msleep(uint64_t ms) {
    sim::advance_by(ms * sim::TICKS_PER_MS);
}

// ---- pad injection: bits are the FPGA's own pad inputs; the core reports
// them back to the firmware in change-detect 0x03 frames, like hardware ----
std::mutex g_pad_m;
uint16_t g_pad_bits = 0; // currently driven bits

void drive_pads() {
    uint16_t bits;
    {
        std::lock_guard<std::mutex> lk(g_pad_m);
        bits = g_pad_bits;
    }
    fpga::set_pads(bits, 0);
}

void pads_add(uint16_t bits) {
    {
        std::lock_guard<std::mutex> lk(g_pad_m);
        g_pad_bits |= bits;
    }
    drive_pads();
}

void pads_remove(uint16_t bits) {
    {
        std::lock_guard<std::mutex> lk(g_pad_m);
        g_pad_bits &= (uint16_t)~bits;
    }
    drive_pads();
    // Drop the level from the firmware's view at once (the pad frame queued
    // above still carries the edge for fidelity). Otherwise a menu pass
    // sampling between our observation and the frame's delivery would act on
    // the stale held level a second time.
    if (state_mutex && xSemaphoreTake(state_mutex, portMAX_DELAY) == pdTRUE) {
        joy1_state &= (uint16_t)~bits;
        xSemaphoreGive(state_mutex);
    }
}

// ---- OSD dump for failure reports ----
std::string dump_screen() {
    fpga::OsdSnapshot snap = fpga::osd_snapshot();
    std::string out = "--- OSD (32x28) ---\n";
    for (int r = 0; r < fpga::OSD_ROWS; r++) {
        std::string row(snap.cells[r], fpga::OSD_COLS);
        while (!row.empty() && row.back() == ' ')
            row.pop_back();
        char tag = (snap.cells[r][0] == '>') ? '>' : ' ';
        out += tag;
        out += row.empty() ? "" : row.substr(1);
        out += "\n";
    }
    out += "cursor rows: [";
    auto rows = fpga::cursor_rows();
    for (size_t i = 0; i < rows.size(); i++) {
        char n[16];
        snprintf(n, sizeof(n), "%s%d", i ? "," : "", rows[i]);
        out += n;
    }
    char tail[256];
    snprintf(tail, sizeof(tail), "] core=%d (%s) config=0x%08x overlay=%s\n",
             fpga::programmed_id(), fpga::programmed_file().c_str(),
             fpga::last_config(), _overlay_on ? "on" : "off");
    out += tail;
    return out;
}

[[noreturn]] void script_fail(int lineno, const std::string &msg) {
    fprintf(stderr, "FAIL line %d: %s\n", lineno, msg.c_str());
    fprintf(stderr, "%s", dump_screen().c_str());
    auto logs = fpga::log_tail(15);
    if (!logs.empty()) {
        fprintf(stderr, "--- core log (tail) ---\n");
        for (auto &l : logs)
            fprintf(stderr, "%s\n", l.c_str());
    }
    fflush(stderr);
    fflush(stdout);
    _exit(1);
}

// ---- parsing helpers ----
bool parse_dur(const std::string &s, uint64_t &ms) {
    if (s.size() > 2 && s.substr(s.size() - 2) == "ms") {
        ms = strtoul(s.substr(0, s.size() - 2).c_str(), nullptr, 10);
        return true;
    }
    if (!s.empty() && s.back() == 's') {
        ms = strtoul(s.substr(0, s.size() - 1).c_str(), nullptr, 10) * 1000;
        return true;
    }
    return false;
}

bool parse_num(const std::string &s, long &v) {
    char *end = nullptr;
    v = strtol(s.c_str(), &end, 0);
    return end && *end == 0;
}

bool parse_btns(const std::string &s, uint16_t &bits) {
    bits = 0;
    size_t i = 0;
    auto lower = s;
    for (auto &c : lower)
        c = (char)tolower(c);
    while (i < lower.size()) {
        size_t j = lower.find('+', i);
        std::string w = lower.substr(i, j == std::string::npos ? j : j - i);
        if (w == "b")
            bits |= 0x001;
        else if (w == "y")
            bits |= 0x002;
        else if (w == "select" || w == "sel")
            bits |= 0x004;
        else if (w == "start")
            bits |= 0x008;
        else if (w == "up")
            bits |= 0x010;
        else if (w == "down")
            bits |= 0x020;
        else if (w == "left")
            bits |= 0x040;
        else if (w == "right")
            bits |= 0x080;
        else if (w == "a")
            bits |= 0x100;
        else if (w == "x")
            bits |= 0x200;
        else if (w == "l")
            bits |= 0x400;
        else if (w == "r")
            bits |= 0x800;
        else
            return false;
        if (j == std::string::npos)
            break;
        i = j + 1;
    }
    return bits != 0;
}

int core_id_for_name(const std::string &name, bool &ok) {
    std::string n = name;
    for (auto &c : n)
        c = (char)tolower(c);
    ok = true;
    if (n == "monitor")
        return 0;
    if (n == "nestang")
        return 1;
    if (n == "snestang")
        return 2;
    if (n == "gbatang")
        return 3;
    if (n == "mdtang")
        return 4;
    if (n == "smstang")
        return 5;
    if (n == "pctang")
        return 6;
    ok = false;
    return -1;
}

// Tokenize a script line, honoring double quotes.
std::vector<std::string> tokenize(const std::string &line) {
    std::vector<std::string> out;
    std::string cur;
    bool in_q = false;
    for (size_t i = 0; i < line.size(); i++) {
        char c = line[i];
        if (c == '"') {
            in_q = !in_q;
            continue;
        }
        if (!in_q && (c == ' ' || c == '\t')) {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
            continue;
        }
        cur += c;
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

bool poll_until(uint64_t timeout_ms, bool (*cond)(void *), void *arg) {
    uint64_t t0 = now_ms();
    for (;;) {
        if (cond(arg))
            return true;
        if (now_ms() - t0 >= timeout_ms)
            return false;
        // Small chunks: time flows (firmware delays expire, the RTL model
        // steps) while the condition is rechecked promptly.
        msleep(5);
    }
}

// ---- script press/hold primitives ----
// press: drive the buttons until the OSD visibly reacts (cursor moves or
// content changes), then release. The firmware steps once per input-loop
// pass and then debounces ~100 ms, so releasing promptly after the first
// visible reaction gives exactly one step.
//
// Two things make this deterministic (independent of host scheduling):
//  - While waiting, the script never advances the clock itself: it only
//    yields, so its observe-release window stays microseconds of wall time.
//    The rate cap (yield_wall) stretches the firmware's 100 ms debounce to
//    ~10 ms of wall time, which cannot elapse unseen in that window. A
//    periodic tiny advance is only a backstop in case time ever stops
//    flowing.
//  - On release the pressed bits are cleared straight in the firmware's pad
//    state (under state_mutex), as well as through the normal pad frame, so
//    a menu pass sampling after the release can never see the level held.
void cmd_press(uint16_t bits) {
    msleep(100); // let the previous screen settle
    auto rows0 = fpga::cursor_rows();
    uint64_t h0 = fpga::osd_hash();
    pads_add(bits);
    uint64_t t0 = now_ms();
    int spin = 0;
    for (;;) {
        if (fpga::cursor_rows() != rows0 || fpga::osd_hash() != h0)
            break;
        if (now_ms() - t0 > 2000)
            break; // no reaction (e.g. at list end): release anyway
        if (++spin % 20000 == 0)
            msleep(5); // backstop: keep time flowing no matter what
        else
            std::this_thread::yield();
    }
    pads_remove(bits);
    msleep(300);
}

void cmd_hold(uint16_t bits, uint64_t ms) {
    msleep(100);
    pads_add(bits);
    msleep(ms);
    pads_remove(bits);
    msleep(150);
}

// ---- reboot: re-exec ourselves, resuming the script after `line` ----
[[noreturn]] void reboot_resume(long line) {
    char pos[32];
    snprintf(pos, sizeof(pos), "%ld", line);
    std::string self = g_argv0;
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = 0;
        self = exe;
    }
    fflush(stdout);
    fflush(stderr);
    if (g_script.empty()) {
        execl(self.c_str(), self.c_str(), "--sd", g_sdroot.c_str(), "--core",
              g_core.c_str(), (char *)nullptr);
    } else {
        execl(self.c_str(), self.c_str(), "--sd", g_sdroot.c_str(), "--core",
              g_core.c_str(), "--script", g_script.c_str(), "--script-pos", pos,
              (char *)nullptr);
    }
    perror("execl");
    exit(1);
}

// ---- script runner ----
struct CondStr {
    const std::string *s;
};
struct CondRow {
    int row;
};
struct CondCore {
    int id;
};
struct CondBit {
    int bit, val;
};
struct CondOverlay {
    bool on;
};
struct CondFile {
    std::string path;
    long size;
};
struct CondNoFile {
    std::string path;
};
struct CondSaveRam {
    long off, val;
};

bool c_screen(void *a) {
    return fpga::screen_contains(*(const std::string *)a);
}
bool c_cursor(void *a) {
    int row = *(int *)a;
    for (int r : fpga::cursor_rows())
        if (r == row)
            return true;
    return false;
}
bool c_core(void *a) {
    return fpga::programmed_id() == *(int *)a;
}
bool c_bit(void *a) {
    CondBit *c = (CondBit *)a;
    return (int)((fpga::last_config() >> c->bit) & 1) == c->val;
}
bool c_overlay(void *a) {
    return (_overlay_on != 0) == *(bool *)a;
}
bool c_file(void *a) {
    CondFile *c = (CondFile *)a;
    struct stat st;
    std::string p = g_sdroot + "/" + c->path;
    return stat(p.c_str(), &st) == 0 && (long)st.st_size == c->size;
}
bool c_nofile(void *a) {
    struct stat st;
    std::string p = g_sdroot + "/" + *(std::string *)a;
    return stat(p.c_str(), &st) != 0;
}
bool c_saveram(void *a) {
    CondSaveRam *c = (CondSaveRam *)a;
    return fpga::save_ram_byte((uint16_t)c->off) == (uint8_t)c->val;
}

int run_script() {
    FILE *f = fopen(g_script.c_str(), "r");
    if (!f) {
        fprintf(stderr, "cannot open script %s\n", g_script.c_str());
        fflush(stderr);
        _exit(2);
    }
    std::vector<std::string> lines;
    char buf[1024];
    while (fgets(buf, sizeof(buf), f))
        lines.push_back(buf);
    fclose(f);

    for (long i = g_script_pos; i < (long)lines.size(); i++) {
        std::string line = lines[(size_t)i];
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();
        size_t ns = line.find_first_not_of(" \t");
        if (ns == std::string::npos || line[ns] == '#')
            continue;
        auto t = tokenize(line.substr(ns));
        if (t.empty())
            continue;
        int lineno = (int)i + 1;
        g_current_line.store((int)i);
        const std::string &cmd = t[0];

        if (cmd == "wait") {
            uint64_t ms;
            if (t.size() != 2 || !parse_dur(t[1], ms))
                script_fail(lineno, "usage: wait <dur>");
            printf("[%d] wait %s\n", lineno, t[1].c_str());
            msleep(ms);
        } else if (cmd == "press") {
            uint16_t bits;
            if (t.size() != 2 || !parse_btns(t[1], bits))
                script_fail(lineno, "usage: press <buttons>");
            printf("[%d] press %s\n", lineno, t[1].c_str());
            cmd_press(bits);
        } else if (cmd == "hold") {
            uint16_t bits;
            uint64_t ms;
            if (t.size() != 3 || !parse_btns(t[1], bits) || !parse_dur(t[2], ms))
                script_fail(lineno, "usage: hold <buttons> <dur>");
            printf("[%d] hold %s %s\n", lineno, t[1].c_str(), t[2].c_str());
            cmd_hold(bits, ms);
        } else if (cmd == "expect-screen" || cmd == "expect-no-screen") {
            bool neg = cmd == "expect-no-screen";
            uint64_t to = neg ? 2000 : 10000;
            if (t.size() < 2 || t.size() > 3)
                script_fail(lineno, "usage: expect-screen \"text\" [timeout]");
            if (t.size() == 3 && !parse_dur(t[2], to))
                script_fail(lineno, "bad timeout");
            std::string s = t[1];
            printf("[%d] %s \"%s\"\n", lineno, cmd.c_str(), s.c_str());
            if (neg) {
                uint64_t t0 = now_ms();
                bool seen = false;
                while (now_ms() - t0 < to) {
                    if (fpga::screen_contains(s)) {
                        seen = true;
                        break;
                    }
                    msleep(20);
                }
                if (seen)
                    script_fail(lineno, "unexpected screen text: " + s);
            } else if (!poll_until(to, c_screen, &s)) {
                script_fail(lineno, "screen never showed: " + s);
            }
        } else if (cmd == "expect-cursor-row") {
            long row;
            uint64_t to = 10000;
            if (t.size() < 2 || t.size() > 3 || !parse_num(t[1], row))
                script_fail(lineno, "usage: expect-cursor-row <n> [timeout]");
            if (t.size() == 3 && !parse_dur(t[2], to))
                script_fail(lineno, "bad timeout");
            printf("[%d] expect-cursor-row %ld\n", lineno, row);
            int r = (int)row;
            if (!poll_until(to, c_cursor, &r))
                script_fail(lineno, "cursor never reached row " + t[1]);
        } else if (cmd == "expect-core") {
            bool ok;
            int id = 0;
            uint64_t to = 10000;
            if (t.size() < 2 || t.size() > 3)
                script_fail(lineno, "usage: expect-core <name> [timeout]");
            id = core_id_for_name(t[1], ok);
            if (!ok)
                script_fail(lineno, "unknown core: " + t[1]);
            if (t.size() == 3 && !parse_dur(t[2], to))
                script_fail(lineno, "bad timeout");
            printf("[%d] expect-core %s\n", lineno, t[1].c_str());
            if (!poll_until(to, c_core, &id))
                script_fail(lineno, "core never became " + t[1]);
        } else if (cmd == "expect-config-bit") {
            long bit, val;
            uint64_t to = 10000;
            if (t.size() < 3 || t.size() > 4 || !parse_num(t[1], bit) ||
                !parse_num(t[2], val))
                script_fail(lineno, "usage: expect-config-bit <bit> <0|1> [timeout]");
            if (t.size() == 4 && !parse_dur(t[3], to))
                script_fail(lineno, "bad timeout");
            CondBit c{(int)bit, (int)val};
            printf("[%d] expect-config-bit %ld %ld\n", lineno, bit, val);
            if (!poll_until(to, c_bit, &c))
                script_fail(lineno, "config bit never matched");
        } else if (cmd == "expect-overlay") {
            uint64_t to = 10000;
            if (t.size() < 2 || t.size() > 3)
                script_fail(lineno, "usage: expect-overlay <on|off> [timeout]");
            bool on = t[1] == "on";
            if (t[1] != "on" && t[1] != "off")
                script_fail(lineno, "usage: expect-overlay <on|off> [timeout]");
            if (t.size() == 3 && !parse_dur(t[2], to))
                script_fail(lineno, "bad timeout");
            printf("[%d] expect-overlay %s\n", lineno, t[1].c_str());
            if (!poll_until(to, c_overlay, &on))
                script_fail(lineno, std::string("overlay never ") + t[1].c_str());
        } else if (cmd == "expect-file") {
            // expect-file <rel> size <n> [timeout]
            uint64_t to = 15000;
            long n;
            if (t.size() < 4 || t.size() > 5 || t[2] != "size" || !parse_num(t[3], n))
                script_fail(lineno, "usage: expect-file <rel> size <n> [timeout]");
            if (t.size() == 5 && !parse_dur(t[4], to))
                script_fail(lineno, "bad timeout");
            CondFile c{t[1], n};
            printf("[%d] expect-file %s size %ld\n", lineno, t[1].c_str(), n);
            if (!poll_until(to, c_file, &c))
                script_fail(lineno, "file never appeared with size: " + t[1]);
        } else if (cmd == "expect-no-file") {
            uint64_t to = 2000;
            if (t.size() < 2 || t.size() > 3)
                script_fail(lineno, "usage: expect-no-file <rel> [timeout]");
            if (t.size() == 3 && !parse_dur(t[2], to))
                script_fail(lineno, "bad timeout");
            printf("[%d] expect-no-file %s\n", lineno, t[1].c_str());
            uint64_t t0 = now_ms();
            bool seen = false;
            while (now_ms() - t0 < to) {
                if (!c_nofile(&t[1])) {
                    seen = true;
                    break;
                }
                msleep(20);
            }
            if (seen)
                script_fail(lineno, "unexpected file appeared: " + t[1]);
        } else if (cmd == "expect-save-ram") {
            long off, val;
            uint64_t to = 10000;
            if (t.size() < 3 || t.size() > 4 || !parse_num(t[1], off) ||
                !parse_num(t[2], val))
                script_fail(lineno, "usage: expect-save-ram <off> <byte> [timeout]");
            if (t.size() == 4 && !parse_dur(t[3], to))
                script_fail(lineno, "bad timeout");
            CondSaveRam c{off, val};
            printf("[%d] expect-save-ram 0x%lx 0x%lx\n", lineno, off, val);
            if (!poll_until(to, c_saveram, &c))
                script_fail(lineno, "save RAM never matched");
        } else if (cmd == "expect-alive") {
            uint64_t ms;
            if (t.size() != 2 || !parse_dur(t[1], ms))
                script_fail(lineno, "usage: expect-alive <dur>");
            printf("[%d] expect-alive %s\n", lineno, t[1].c_str());
            uint32_t h0 = heartbeat;
            msleep(ms);
            uint32_t h1 = heartbeat;
            if (h1 == h0)
                script_fail(lineno, "heartbeat did not move (watchdog would fire)");
        } else if (cmd == "poke-save") {
            long off, val;
            if (t.size() != 3 || !parse_num(t[1], off) || !parse_num(t[2], val))
                script_fail(lineno, "usage: poke-save <off> <byte>");
            printf("[%d] poke-save 0x%lx 0x%lx\n", lineno, off, val);
            fpga::poke_save((uint16_t)off, (uint8_t)val);
        } else if (cmd == "wram-write") {
            long off, val;
            if (t.size() != 3 || !parse_num(t[1], off) || !parse_num(t[2], val))
                script_fail(lineno, "usage: wram-write <off> <byte>");
            printf("[%d] wram-write 0x%lx 0x%lx\n", lineno, off, val);
            fpga::wram_write((uint16_t)off, (uint8_t)val);
        } else if (cmd == "wram-burst") {
            long off, len, seed;
            if (t.size() != 4 || !parse_num(t[1], off) || !parse_num(t[2], len) ||
                !parse_num(t[3], seed))
                script_fail(lineno, "usage: wram-burst <off> <len> <seed>");
            printf("[%d] wram-burst 0x%lx len %ld seed 0x%lx\n", lineno, off, len, seed);
            fpga::wram_burst((uint16_t)off, (uint16_t)len, (uint8_t)seed);
        } else if (cmd == "mode" || cmd == "mode-now") {
            long ms = 4000;
            if (t.size() > 2 || (t.size() == 2 && !parse_num(t[1], ms)))
                script_fail(lineno, "usage: mode|mode-now [silence-ms]");
            printf("[%d] %s (MODE button: FPGA reloads from flash)\n", lineno, cmd.c_str());
            // `mode` presses once the firmware has polled the core since its
            // last programming (its first poll is ~2 s after boot or
            // reprogramming). `mode-now` presses right away, inside that
            // window, where only the core-0 answer gives MODE away.
            if (cmd == "mode")
                while (sim_uptime_ms() < 3000 || fpga::ms_since_program() < 2500)
                    msleep(50);
            fpga::trigger_mode((int)ms);
            // The firmware restarts itself; the supervisor re-execs us past
            // this line. If nothing happens, the expectation fails here.
            uint64_t t0 = now_ms();
            while (!g_reset_requested.load() && now_ms() - t0 < 12000)
                msleep(20);
            if (!g_reset_requested.load())
                script_fail(lineno, "MODE did not restart the firmware");
            for (;;)
                msleep(1000); // supervisor takes over (re-exec)
        } else if (cmd == "power-cycle") {
            printf("[%d] power-cycle: rebooting\n", lineno);
            fflush(stdout);
            reboot_resume(i + 1);
        } else if (cmd == "host-rm") {
            if (t.size() != 2)
                script_fail(lineno, "usage: host-rm <relpath>");
            std::string p = g_sdroot + "/" + t[1];
            printf("[%d] host-rm %s\n", lineno, t[1].c_str());
            struct stat st;
            if (stat(p.c_str(), &st) == 0 && unlink(p.c_str()) != 0)
                script_fail(lineno, "host-rm failed: " + t[1]);
        } else if (cmd == "echo") {
            printf("[%d] %s\n", lineno, line.substr(ns + 4).c_str());
        } else {
            script_fail(lineno, "unknown command: " + cmd);
        }
    }
    printf("SCRIPT PASS (%s)\n", g_script.c_str());
    fflush(stdout);
    _exit(0);
}

// ---- interactive mode ----
struct TermGuard {
    termios saved;
    bool active = false;
    void raw() {
        if (tcgetattr(STDIN_FILENO, &saved) != 0)
            return;
        termios t = saved;
        t.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 1;
        tcsetattr(STDIN_FILENO, TCSANOW, &t);
        active = true;
    }
    ~TermGuard() {
        if (active)
            tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    }
};

void render_screen() {
    fpga::OsdSnapshot snap = fpga::osd_snapshot();
    printf("\x1b[2J\x1b[H");
    printf("TangCore sim  core=%d(%s) cfg=0x%08x hb=%u wdg=%llu\n", fpga::programmed_id(),
           fpga::programmed_file().c_str(), fpga::last_config(), heartbeat,
           (unsigned long long)sim_wdg_feeds());
    for (int r = 0; r < fpga::OSD_ROWS; r++)
        printf("|%.32s|\n", snap.cells[r]);
    printf("keys: arrows/z=B x=A a=Y s=X enter=START bksp=SEL q=L w=R | g=menu t=reset T=close m=MODE p=reboot Q=quit\n");
    fflush(stdout);
}

void pulse(uint16_t bits, uint64_t ms) {
    pads_add(bits);
    msleep(ms);
    pads_remove(bits);
}

int run_interactive() {
    TermGuard term;
    term.raw();
    printf("booting firmware (SD: %s)...\n", g_sdroot.c_str());
    uint64_t t0 = now_ms();
    while (!fpga::screen_contains("TangCore") && now_ms() - t0 < 15000)
        msleep(50);
    std::string esc;
    uint64_t last_render = 0;
    for (;;) {
        if (g_reset_requested.load()) {
            printf("\nfirmware reset: rebooting...\n");
            fflush(stdout);
            reboot_resume(0);
        }
        if (now_ms() - last_render > 120) {
            render_screen();
            last_render = now_ms();
        }
        char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n <= 0) {
            msleep(10);
            continue;
        }
        if (c == 0x1b) {
            char seq[2] = {0, 0};
            if (read(STDIN_FILENO, &seq[0], 1) == 1 && seq[0] == '[' &&
                read(STDIN_FILENO, &seq[1], 1) == 1) {
                if (seq[1] == 'A')
                    pulse(0x010, 150);
                else if (seq[1] == 'B')
                    pulse(0x020, 150);
                else if (seq[1] == 'C')
                    pulse(0x080, 150);
                else if (seq[1] == 'D')
                    pulse(0x040, 150);
            } else {
                break; // plain Esc: quit
            }
        } else if (c == 'z' || c == 'Z')
            pulse(0x001, 150);
        else if (c == 'x' || c == 'X')
            pulse(0x100, 150);
        else if (c == 'a' || c == 'A')
            pulse(0x002, 150);
        else if (c == 's' || c == 'S')
            pulse(0x200, 150);
        else if (c == '\r' || c == '\n')
            pulse(0x008, 150);
        else if (c == 0x7f || c == 0x08 || c == '\t')
            pulse(0x004, 150);
        else if (c == 'q')
            pulse(0x400, 150);
        else if (c == 'w')
            pulse(0x800, 150);
        else if (c == 'g')
            pulse(0x004 | 0x008 | 0x400, 500); // menu combo
        else if (c == 't')
            pulse(0x004 | 0x008 | 0x800, 500); // reset combo
        else if (c == 'T')
            pulse(0x004 | 0x008 | 0x800, 3500); // held: close game
        else if (c == 'm') {
            printf("\nMODE pressed: FPGA reloading...\n");
            if (sim_uptime_ms() < 3000 || fpga::ms_since_program() < 2500) {
                printf("(waiting out the boot stretch...)\n");
                while (sim_uptime_ms() < 3000 || fpga::ms_since_program() < 2500)
                    msleep(50);
            }
            fpga::trigger_mode(4000);
        } else if (c == 'p') {
            printf("\nrebooting...\n");
            fflush(stdout);
            reboot_resume(0);
        } else if (c == 'Q') {
            break;
        }
    }
    return 0;
}

void supervisor() {
    // Re-exec on firmware-requested reset (MODE button, flash mode path).
    // Time keeps flowing while watching: the rebooting firmware may still
    // need its delays to expire (e.g. MODE detection after the silence).
    for (;;) {
        if (!g_reset_requested.load()) {
            msleep(50);
            continue;
        }
        if (g_script.empty()) {
            printf("firmware reset: rebooting\n");
            fflush(stdout);
            reboot_resume(0);
        }
        long resume = (long)g_current_line.load() + 1;
        printf("firmware reset: rebooting, resuming script at line %ld\n", resume + 1);
        fflush(stdout);
        msleep(200); // let the dust settle (UART queues die with exec)
        reboot_resume(resume);
    }
}

} // namespace

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    g_argv0 = argv[0];
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--sd" && i + 1 < argc)
            g_sdroot = argv[++i];
        else if (a == "--core" && i + 1 < argc)
            g_core = argv[++i];
        else if (a == "--script" && i + 1 < argc)
            g_script = argv[++i];
        else if (a == "--script-pos" && i + 1 < argc)
            g_script_pos = strtol(argv[++i], nullptr, 10);
        else if (a == "--verbose")
            g_verbose = true;
        else {
            fprintf(stderr, "usage: %s --sd <dir> [--core fake|nestang-rtl] [--script <file>] [--verbose]\n",
                    argv[0]);
            return 2;
        }
    }
    if (g_sdroot.empty()) {
        fprintf(stderr, "usage: %s --sd <dir> [--core fake|nestang-rtl] [--script <file>] [--verbose]\n",
                argv[0]);
        return 2;
    }
    struct stat st;
    if (stat(g_sdroot.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "SD dir missing: %s\n", g_sdroot.c_str());
        return 2;
    }

    if (g_core == "fake") {
        fpga_use_fake();
    } else if (g_core == "nestang-rtl") {
        FpgaBackend *rtl = fpga_rtl_backend();
        if (!rtl) {
            fprintf(stderr, "RTL backend not built in (needs docker + NESTANG_DIR at build time)\n");
            return 2;
        }
        fpga_select(rtl);
    } else {
        fprintf(stderr, "unknown core backend: %s\n", g_core.c_str());
        return 2;
    }
    printf("backend: %s\n", fpga::backend_name());

    sim_rtos::init_timebase();
    fpga::set_verbose(g_verbose);
    ffsim_set_root(g_sdroot.c_str());
    fpga::start();

    std::thread(firmware_main).detach();

    // Wait for the firmware to create its shared-state mutex. The firmware
    // drives sim time with its own delays, so this wait terminates even if
    // the script never advances the clock.
    uint64_t t0 = now_ms();
    while (state_mutex == nullptr && now_ms() - t0 < 10000)
        msleep(5);
    if (!state_mutex) {
        fprintf(stderr, "firmware did not start\n");
        fflush(stderr);
        _exit(1);
    }

    std::thread(supervisor).detach();

    if (!g_script.empty()) {
        if (g_script_pos > 0)
            printf("resuming script %s at line %ld\n", g_script.c_str(), g_script_pos + 1);
        run_script(); // _exits
        _exit(1);
    }
    run_interactive(); // _exits
    _exit(0);
}
