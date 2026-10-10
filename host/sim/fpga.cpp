// Fake FPGA core implementation. See fpga.hpp.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "fpga.hpp"

namespace fpga {
namespace {

std::mutex g_tx_m;
std::condition_variable g_tx_cv;
std::deque<uint8_t> g_tx;

std::mutex g_rx_m;
std::deque<uint8_t> g_rx;

std::recursive_mutex g_st_m;
OsdSnapshot g_osd;
int g_core_id = -1;
std::string g_core_file;
uint32_t g_config = 0;
bool g_overlay_visible = true;
uint64_t g_rom_bytes = 0;
std::vector<uint8_t> g_save_ram;
std::atomic<uint64_t> g_silence_until_ms{0};
std::atomic<uint64_t> g_program_ms{0};
std::atomic<bool> g_verbose{false};
std::vector<std::string> g_log;
std::string g_log_cur;

uint64_t steady_ms() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void vlog(const std::string &s) {
    if (g_verbose)
        fprintf(stderr, "[fpga] %s\n", s.c_str());
    if (g_log.size() > 500)
        g_log.erase(g_log.begin());
    g_log.push_back(s);
}

void rx_push_frame(uint8_t type, const uint8_t *payload, size_t n) {
    // Frame: AA lenH lenL type payload[len-1]. len counts type+payload.
    uint16_t len = (uint16_t)(n + 1);
    std::lock_guard<std::mutex> lk(g_rx_m);
    g_rx.push_back(0xAA);
    g_rx.push_back((uint8_t)(len >> 8));
    g_rx.push_back((uint8_t)(len & 0xff));
    g_rx.push_back(type);
    for (size_t i = 0; i < n; i++)
        g_rx.push_back(payload[i]);
}

bool silenced() {
    return steady_ms() < g_silence_until_ms.load();
}

void osd_put(uint8_t b) {
    if (b == '\n') {
        g_osd.cursor_col = 0;
        g_osd.cursor_row++;
        return;
    }
    if (b == '\r') {
        g_osd.cursor_col = 0;
        return;
    }
    if (g_osd.cursor_row < OSD_ROWS && g_osd.cursor_col < OSD_COLS)
        g_osd.cells[g_osd.cursor_row][g_osd.cursor_col] = (char)b;
    g_osd.cursor_col++;
    if (g_osd.cursor_col >= OSD_COLS) {
        g_osd.cursor_col = 0;
        g_osd.cursor_row++;
    }
}

void dispatch(uint8_t type, const std::vector<uint8_t> &p) {
    std::lock_guard<std::recursive_mutex> lk(g_st_m);
    switch (type) {
    case 0x01: { // get core ID
        if (silenced() || g_core_id < 0) {
            if (g_verbose)
                fprintf(stderr, "[fpga] 0x01 dropped (id=%d) t=%llu\n", g_core_id,
                        (unsigned long long)steady_ms());
            break;
        }
        uint8_t id = (uint8_t)g_core_id;
        rx_push_frame(0x01, &id, 1);
        break;
    }
    case 0x03: // core_config
        if (p.size() >= 4)
            g_config = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                       ((uint32_t)p[2] << 8) | p[3];
        break;
    case 0x04: // overlay cursor
        if (p.size() >= 2) {
            g_osd.cursor_col = p[0];
            g_osd.cursor_row = p[1];
        }
        break;
    case 0x05: // overlay text
        for (uint8_t b : p)
            osd_put(b);
        break;
    case 0x06: // loading state: nothing to track beyond the log
        break;
    case 0x07: // ROM data
        g_rom_bytes += p.size();
        break;
    case 0x08: // overlay on/off
        if (!p.empty())
            g_overlay_visible = p[0] != 0;
        break;
    case 0x09: // joypad state: nothing to do with it
        break;
    case 0x0b: // PC/XT port write: not emulated
        break;
    case 0x0d: // dprint: keep as log text
        for (uint8_t b : p) {
            if (b == '\n') {
                vlog("core: " + g_log_cur);
                g_log_cur.clear();
            } else {
                g_log_cur += (char)b;
            }
        }
        break;
    case 0x11: { // save restore: blk16 + 512 bytes
        if (p.size() < 2 + 512)
            break;
        uint16_t blk = ((uint16_t)p[0] << 8) | p[1];
        size_t off = (size_t)blk * 512;
        if (off + 512 <= g_save_ram.size())
            memcpy(g_save_ram.data() + off, p.data() + 2, 512);
        break;
    }
    case 0x12: { // save request: answer with a 0x0A block
        if (p.size() < 2 || silenced())
            break;
        uint16_t blk = ((uint16_t)p[0] << 8) | p[1];
        size_t off = (size_t)blk * 512;
        uint8_t blkbytes[2] = {(uint8_t)(blk >> 8), (uint8_t)(blk & 0xff)};
        static uint8_t blank[512];
        memset(blank, 0xFF, sizeof(blank));
        const uint8_t *data = blank;
        if (off + 512 <= g_save_ram.size())
            data = g_save_ram.data() + off;
        uint8_t payload[2 + 512];
        payload[0] = blkbytes[0];
        payload[1] = blkbytes[1];
        memcpy(payload + 2, data, 512);
        rx_push_frame(0x0A, payload, sizeof(payload));
        break;
    }
    default:
        break;
    }
}

void thread_main() {
    enum { WAIT_AA, LEN_H, LEN_L, TYPE, PAYLOAD } st = WAIT_AA;
    uint16_t len = 0;
    uint8_t type = 0;
    std::vector<uint8_t> payload;
    for (;;) {
        uint8_t b;
        {
            std::unique_lock<std::mutex> lk(g_tx_m);
            g_tx_cv.wait(lk, [] { return !g_tx.empty(); });
            b = g_tx.front();
            g_tx.pop_front();
        }
        switch (st) {
        case WAIT_AA:
            if (b == 0xAA)
                st = LEN_H;
            break;
        case LEN_H:
            len = (uint16_t)b << 8;
            st = LEN_L;
            break;
        case LEN_L:
            len |= b;
            st = TYPE;
            break;
        case TYPE:
            type = b;
            payload.clear();
            if (len <= 1) {
                dispatch(type, payload);
                st = WAIT_AA;
            } else {
                payload.reserve(len - 1);
                st = PAYLOAD;
            }
            break;
        case PAYLOAD:
            payload.push_back(b);
            if (payload.size() >= (size_t)(len - 1)) {
                dispatch(type, payload);
                st = WAIT_AA;
            }
            break;
        }
    }
}

int id_for_file(const std::string &path) {
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

} // namespace

void start() {
    {
        std::lock_guard<std::recursive_mutex> lk(g_st_m);
        memset(g_osd.cells, ' ', sizeof(g_osd.cells));
        g_osd.cursor_col = 0;
        g_osd.cursor_row = 0;
        if (g_save_ram.empty())
            g_save_ram.assign(256 * 512, 0xFF);
    }
    std::thread(thread_main).detach();
}

void tx_push(uint8_t b) {
    {
        std::lock_guard<std::mutex> lk(g_tx_m);
        g_tx.push_back(b);
    }
    g_tx_cv.notify_one();
}

bool rx_available() {
    std::lock_guard<std::mutex> lk(g_rx_m);
    return !g_rx.empty();
}

uint8_t rx_pop() {
    std::lock_guard<std::mutex> lk(g_rx_m);
    if (g_rx.empty())
        return 0;
    uint8_t b = g_rx.front();
    g_rx.pop_front();
    return b;
}

void set_programmed(const char *path) {
    std::lock_guard<std::recursive_mutex> lk(g_st_m);
    g_core_file = path ? path : "";
    g_core_id = id_for_file(g_core_file);
    g_program_ms.store(steady_ms(), std::memory_order_relaxed);
}

uint64_t ms_since_program() {
    uint64_t p = g_program_ms.load(std::memory_order_relaxed);
    if (!p)
        return 0xFFFFFFFFull;
    return steady_ms() - p;
}

int programmed_id() {
    std::lock_guard<std::recursive_mutex> lk(g_st_m);
    return g_core_id;
}

std::string programmed_file() {
    std::lock_guard<std::recursive_mutex> lk(g_st_m);
    return g_core_file;
}

uint32_t last_config() {
    std::lock_guard<std::recursive_mutex> lk(g_st_m);
    return g_config;
}

OsdSnapshot osd_snapshot() {
    std::lock_guard<std::recursive_mutex> lk(g_st_m);
    return g_osd;
}

uint64_t osd_hash() {
    std::lock_guard<std::recursive_mutex> lk(g_st_m);
    uint64_t h = 1469598103934665603ull;
    for (int r = 0; r < OSD_ROWS; r++)
        for (int c = 0; c < OSD_COLS; c++) {
            h ^= (uint64_t)(uint8_t)g_osd.cells[r][c];
            h *= 1099511628211ull;
        }
    return h;
}

std::vector<int> cursor_rows() {
    std::lock_guard<std::recursive_mutex> lk(g_st_m);
    std::vector<int> rows;
    for (int r = 0; r < OSD_ROWS; r++)
        if (g_osd.cells[r][0] == '>')
            rows.push_back(r);
    return rows;
}

bool screen_contains(const std::string &s) {
    if (s.empty())
        return true;
    std::lock_guard<std::recursive_mutex> lk(g_st_m);
    for (int r = 0; r < OSD_ROWS; r++) {
        std::string row(g_osd.cells[r], OSD_COLS);
        if (row.find(s) != std::string::npos)
            return true;
    }
    return false;
}

void poke_save(uint16_t off, uint8_t val) {
    {
        std::lock_guard<std::recursive_mutex> lk(g_st_m);
        if (off < g_save_ram.size())
            g_save_ram[off] = val;
    }
    uint8_t pad = 0;
    rx_push_frame(0x0B, &pad, 1);
}

uint8_t save_ram_byte(uint16_t off) {
    std::lock_guard<std::recursive_mutex> lk(g_st_m);
    if (off < g_save_ram.size())
        return g_save_ram[off];
    return 0;
}

void trigger_mode(int silence_ms) {
    {
        std::lock_guard<std::recursive_mutex> lk(g_st_m);
        // The FPGA reloads its flash bitstream: core 0 answers again after
        // the silent stretch.
        g_core_id = 0;
        g_core_file = "<flash>";
    }
    g_silence_until_ms.store(steady_ms() + (uint64_t)silence_ms);
}

bool overlay_visible() {
    std::lock_guard<std::recursive_mutex> lk(g_st_m);
    return g_overlay_visible;
}

uint64_t rom_bytes() {
    std::lock_guard<std::recursive_mutex> lk(g_st_m);
    return g_rom_bytes;
}

void set_verbose(bool v) {
    g_verbose.store(v);
}

std::vector<std::string> log_tail(int n) {
    std::lock_guard<std::recursive_mutex> lk(g_st_m);
    if (n <= 0 || n > (int)g_log.size())
        n = (int)g_log.size();
    return std::vector<std::string>(g_log.end() - n, g_log.end());
}

} // namespace fpga
