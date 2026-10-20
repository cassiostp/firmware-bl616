// Save states. See savestates.h for the protocol and file format.
//
// The transfer sequences mirror saves.cpp's dump/restore (same 512-byte
// block shape, same heartbeat and tmp+rename rules), but the SS channel is
// separate from the battery one: commands 0x18/0x19/0x1A, frames 0x0E/0x0F,
// files `<rom>.ss1`..`ss4` beside `<rom>.sav`. Nothing here runs unless
// the running core answered the probe, so old core builds are unaffected
// (they consume 0x18-0x1A as unknown frames and stay silent).
//
// Two-phase load, so a bad file can never touch the running game:
//   1. firmware reads and checks the whole file (header fields, ROM
//      identity, payload CRC) before any FPGA command;
//   2. begin-load freezes the core, block 0 is sent, the core checks it
//      again and only then resets and accepts payload. A reject in either
//      phase just unfreezes (abort) with the game intact.
// Only a link failure *during* payload leaves the machine indeterminate;
// the menu then resets the game (the battery save was settled first).
#include <cstring>
#include <string>
#include <cstdio>

#include "utils.h"
#include "overlay.h"
#include "savestates.h"
#include "fastload.h"

namespace {

constexpr int kSlots = 4;
constexpr uint8_t kFormatVer = 1;
// Per-core state layout versions, keyed by core id (monitortang core-id
// space). The core echoes its id + version in the header and the status
// frame; both are checked, so a state is only ever loaded on the core
// build whose layout produced it.
constexpr uint8_t ss_core_state_ver(int core_id) {
    switch (core_id) {
    case 1: return 1;   // NES
    case 5: return 1;   // SMS/GG
    default: return 0;  // unknown: never matches a core's echo
    }
}

// Per-block waits: a block is 512 B at 200 KB/s wire plus the engine's
// SDRAM time while frozen -- milliseconds. 1 s is generous headroom; the
// load-block wait is only the status poll between phases.
constexpr int kStatusTimeoutMs = 150;
constexpr int kBlockTimeoutMs = 1000;
constexpr int kReadyTimeoutMs = 3000;

std::string ss_base[kSlots];        // ".../<rom>.ssN" (empty: no game)
std::string ss_saves_dir;           // "<drv>saves/<geom>", for mkdirs
int ss_core = -1;
uint32_t ss_rom_size = 0, ss_rom_crc = 0;
bool ss_rom_id_known = false;
bool ss_probed = false;
bool ss_ok = false;                 // probe: core + mapper support states
uint64_t ss_flags = 0;              // probe: running game's mapper_flags
uint32_t ss_total_bytes = 0;        // probe: state image size for this game

SemaphoreHandle_t ss_sem = nullptr; // one binary semaphore, both frame kinds
SemaphoreHandle_t ss_sem_init(void) {
    if (!ss_sem) ss_sem = xSemaphoreCreateBinary();
    return ss_sem;
}

// RX-task frame sinks. Status is copied whole; a block is assembled into
// ss_rx and completed with its number (-1: resync/loss, caller times out).
uint8_t ss_rx_status[16];
volatile int ss_rx_status_ok = 0;
USB_NOCACHE_RAM_SECTION uint8_t ss_rx[512];
volatile int ss_rx_block = -1;
volatile int ss_expect_blk = -1;    // block ss_fetch_block is waiting for
USB_NOCACHE_RAM_SECTION uint8_t ss_io[512];
USB_NOCACHE_RAM_SECTION FIL fss;

void ss_drain_sem(void) {
    while (xSemaphoreTake(ss_sem_init(), 0) == pdTRUE) {
    }
}

// 0x18 with an op; every op is answered with a 0x0E status frame.
void ss_send_op(int op) {
    taskENTER_CRITICAL();
    fpga_tx_header(0x18, 2);
    fpga_tx_byte((uint8_t)op);
    taskEXIT_CRITICAL();
    heartbeat_bump();
}

// Send op and wait for the status frame. Fills the parsed fields.
struct SsStatus {
    bool supported, ready, error, active;
    int state;                  // engine FSM state (2 save-ready, 5 load-ready)
    uint16_t total_blocks;
    uint64_t flags;
};

bool ss_op_status(int op, SsStatus *out, int timeout_ms) {
    ss_rx_status_ok = 0;
    ss_drain_sem();
    ss_send_op(op);
    uint64_t t0 = bflb_mtimer_get_time_ms();
    for (;;) {
        int remain = timeout_ms - (int)(bflb_mtimer_get_time_ms() - t0);
        if (remain <= 0) return false;
        // Short slices + heartbeat: this runs on main_task (probe at load
        // and game-menu open), whose watchdog beat must not go stale
        // while a silent old core lets the full timeout run out.
        if (remain > 20) remain = 20;
        if (xSemaphoreTake(ss_sem, pdMS_TO_TICKS(remain)) == pdTRUE &&
            ss_rx_status_ok)
            break;
        heartbeat_bump();
    }
    const uint8_t *p = ss_rx_status;
    out->supported = p[0] & 1;
    out->ready = p[1] & 1;
    out->error = p[1] & 2;
    out->active = p[1] & 4;
    out->state = p[15] & 7;
    out->total_blocks = (uint16_t)(p[4] | (p[5] << 8));
    uint64_t f = 0;
    for (int i = 0; i < 8; i++) f |= (uint64_t)p[6 + i] << (8 * i);
    out->flags = f;
    return p[2] == kFormatVer && p[3] == ss_core_state_ver(ss_core);
}

// Poll query-status until the engine reaches `want_state`. Matching on the
// FSM state (not the level ready bit) cannot alias with the previous phase:
// e.g. after block 0 the header phase also reports ready, but payload is
// only accepted in the load state.
bool ss_wait_state(SsStatus *out, int want_state) {
    uint64_t t0 = bflb_mtimer_get_time_ms();
    while (bflb_mtimer_get_time_ms() - t0 < (uint64_t)kReadyTimeoutMs) {
        if (!ss_op_status(0, out, kStatusTimeoutMs))
            return false;
        if (out->error) return false;
        if (out->state == want_state) return true;
        delay(20);
    }
    return false;
}

// Request block `blk` (0x1A) and wait for its 0x0F frame. Same retry shape
// as saves.cpp's fetch: the frame is dropped whole or never sent, never
// half-assembled, so asking again is always safe.
bool ss_fetch_block(int blk, int timeout_ms) {
    ss_expect_blk = blk;
    for (int attempt = 0; attempt < 3; attempt++) {
        taskENTER_CRITICAL();
        fpga_tx_header(0x1A, 3);
        fpga_tx_byte((uint8_t)(blk >> 8));
        fpga_tx_byte((uint8_t)blk);
        taskEXIT_CRITICAL();
        ss_rx_block = -1;
        ss_drain_sem();
        heartbeat_bump();
        uint64_t t0 = bflb_mtimer_get_time_ms();
        while (bflb_mtimer_get_time_ms() - t0 < (uint64_t)timeout_ms) {
            int remain = timeout_ms - (int)(bflb_mtimer_get_time_ms() - t0);
            if (remain > 20) remain = 20;
            if (xSemaphoreTake(ss_sem, pdMS_TO_TICKS(remain)) == pdTRUE &&
                ss_rx_block == blk)
                return true;
            heartbeat_bump();
        }
        DEBUG("savestates: block %d fetch attempt %d timed out\n", blk, attempt);
    }
    return false;
}

void ss_send_block(int blk, const uint8_t *data) {
    taskENTER_CRITICAL();
    fpga_tx_header(0x19, 3 + 512);
    fpga_tx_byte((uint8_t)(blk >> 8));
    fpga_tx_byte((uint8_t)blk);
    for (int i = 0; i < 512; i++)
        fpga_tx_byte(data[i]);
    taskEXIT_CRITICAL();
    heartbeat_bump();
}

uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

// "<drv>saves/<geom>" + file, same derivation as saves.cpp.
const char *ss_geom_dir(int core_id) {
    switch (core_id) {
    case 1: return "nes";
    case 2: return "snes";
    case 3: return "gba";
    case 4: return "genesis";
    case 5: return "sms";
    default: return nullptr;
    }
}

bool ss_mkdirs(void) {
    if (ss_saves_dir.empty()) return false;
    std::string root = std::string(drv) + "saves";
    FRESULT r1 = f_mkdir(root.c_str());
    FRESULT r2 = f_mkdir(ss_saves_dir.c_str());
    return r1 == FR_OK || r1 == FR_EXIST || r2 == FR_OK || r2 == FR_EXIST;
}

}  // namespace

void savestates_set_game(const char *rom_path, int core_id) {
    savestates_discard();
    const char *geom = ss_geom_dir(core_id);
    if (!geom || !rom_path) return;
    ss_saves_dir = std::string(drv) + "saves/" + geom;
    // Same base-name rule as saves.cpp: last path component, extension off.
    const char *base = strrchr(rom_path, '/');
    base = base ? base + 1 : rom_path;
    std::string stem(base);
    size_t dot = stem.rfind('.');
    if (dot != std::string::npos) stem.resize(dot);
    if (stem.empty()) return;
    for (int i = 0; i < kSlots; i++) {
        char ext[8];
        snprintf(ext, sizeof(ext), ".ss%d", i + 1);
        ss_base[i] = ss_saves_dir + "/" + stem + ext;
    }
    ss_core = core_id;
}

void savestates_set_rom_id(uint32_t size, uint32_t crc32) {
    ss_rom_size = size;
    ss_rom_crc = crc32;
    ss_rom_id_known = true;
}

void savestates_discard(void) {
    for (auto &p : ss_base) p.clear();
    ss_saves_dir.clear();
    ss_core = -1;
    ss_rom_size = ss_rom_crc = 0;
    ss_rom_id_known = false;
    ss_probed = false;
    ss_ok = false;
    ss_flags = 0;
    ss_total_bytes = 0;
}

void savestates_off(void) {
    savestates_discard();
}

void savestates_probe(void) {
    // Once per game (savestates_set_game/discard clears ss_probed). Called
    // lazily from the game menu, NOT from the load path: a silent old core
    // makes this wait out the full timeout, and 150 ms of blocked main_task
    // right after a load would eat the menu-combo hold window.
    if (ss_probed) return;
    ss_probed = true;
    ss_ok = false;
    if (ss_core < 0) return;
    SsStatus st;
    if (!ss_op_status(0, &st, kStatusTimeoutMs)) {
        DEBUG("savestates: core does not answer 0x18 (old build?)\n");
        return;
    }
    ss_flags = st.flags;
    ss_total_bytes = (uint32_t)st.total_blocks * 512u;
    ss_ok = st.supported && st.total_blocks > 1;
    DEBUG("savestates: probe supported=%d total=%u flags=%08x%08x\n",
          ss_ok, (unsigned)ss_total_bytes,
          (unsigned)(ss_flags >> 32), (unsigned)ss_flags);
}

bool savestates_supported(void) {
    return ss_ok;
}

uint32_t savestates_slot_size(int slot) {
    if (slot < 1 || slot > kSlots || ss_base[slot - 1].empty()) return 0;
    return get_file_size(ss_base[slot - 1].c_str());
}

savestate_result savestates_save(int slot) {
    if (slot < 1 || slot > kSlots || ss_base[slot - 1].empty()) return SS_NO_GAME;
    if (!ss_probed) savestates_probe();
    if (!ss_ok) return SS_UNSUPPORTED;
    if (!ss_mkdirs()) return SS_SD_ERROR;

    SsStatus st;
    if (!ss_op_status(1, &st, kStatusTimeoutMs)) {
        ss_send_op(4);
        return SS_LINK_ERROR;
    }
    if (st.state != 2 && !ss_wait_state(&st, 2)) {
        ss_send_op(4);
        return SS_LINK_ERROR;
    }
    uint32_t total = (uint32_t)st.total_blocks * 512u;
    if (total < 1024 || total != ss_total_bytes) {
        ss_send_op(4);
        return SS_LINK_ERROR;
    }
    int blocks = st.total_blocks;

    std::string tmp = ss_base[slot - 1] + ".tmp";
    if (f_open(&fss, tmp.c_str(), FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        ss_send_op(4);
        return SS_SD_ERROR;
    }
    savestate_result res = SS_OK;
    uint32_t pcrc = 0xFFFFFFFFu;
    for (int blk = 0; blk < blocks && res == SS_OK; blk++) {
        if (!ss_fetch_block(blk, kBlockTimeoutMs)) {
            res = SS_LINK_ERROR;
            break;
        }
        memcpy(ss_io, ss_rx, 512);
        if (blk == 0) {
            // Patch the firmware fields into the FPGA's header.
            wr32(ss_io + 28, ss_rom_size);
            wr32(ss_io + 32, ss_rom_crc);
            wr32(ss_io + 36, 0);            // payload CRC patched below
        } else {
            pcrc = fast_crc32_update(pcrc, ss_io, 512);
        }
        UINT bw = 0;
        if (f_write(&fss, ss_io, 512, &bw) != FR_OK || bw != 512)
            res = SS_SD_ERROR;
        if ((blk & 31) == 31)
            overlay_status("Saving state %d/%dK", (blk * 512) >> 10,
                           (int)(total >> 10));
    }
    ss_send_op(res == SS_OK ? 3 : 4);       // end or abort: unfreeze either way
    if (res == SS_OK) {
        // Backpatch the payload CRC (header bytes 36..39), then commit.
        uint8_t c[4];
        wr32(c, pcrc ^ 0xFFFFFFFFu);
        UINT bw = 0;
        if (f_lseek(&fss, 36) != FR_OK ||
            f_write(&fss, c, 4, &bw) != FR_OK || bw != 4 ||
            f_sync(&fss) != FR_OK)
            res = SS_SD_ERROR;
    }
    f_close(&fss);
    heartbeat_bump();
    if (res == SS_OK) {
        f_unlink(ss_base[slot - 1].c_str());
        if (f_rename(tmp.c_str(), ss_base[slot - 1].c_str()) != FR_OK)
            res = SS_SD_ERROR;
    }
    if (res != SS_OK)
        f_unlink(tmp.c_str());
    DEBUG("savestates: save slot %d -> %d\n", slot, (int)res);
    return res;
}

static bool ss_last_disturbed = false;

bool savestates_last_load_disturbed(void) { return ss_last_disturbed; }

savestate_result savestates_load(int slot) {
    ss_last_disturbed = false;
    if (slot < 1 || slot > kSlots || ss_base[slot - 1].empty()) return SS_NO_GAME;
    if (!ss_probed) savestates_probe();
    if (!ss_ok) return SS_UNSUPPORTED;

    const char *path = ss_base[slot - 1].c_str();
    uint32_t fsize = get_file_size(path);
    if (fsize == 0) return SS_EMPTY;
    if (f_open(&fss, path, FA_READ) != FR_OK) return SS_SD_ERROR;

    savestate_result res = SS_OK;
    UINT br = 0;
    static uint8_t hdr[512];
    if (f_read(&fss, hdr, 512, &br) != FR_OK || br != 512) {
        f_close(&fss);
        return SS_CORRUPT;
    }
    uint64_t hflags = 0;
    for (int i = 0; i < 8; i++) hflags |= (uint64_t)hdr[8 + i] << (8 * i);
    uint32_t total = rd32(hdr + 16);
    if (memcmp(hdr, "TCSS", 4) != 0 || hdr[4] != kFormatVer ||
        hdr[5] != ss_core_state_ver(ss_core) || hdr[6] != (uint8_t)ss_core ||
        hflags != ss_flags || total != ss_total_bytes || fsize != total ||
        (ss_rom_id_known &&
         (rd32(hdr + 28) != ss_rom_size || rd32(hdr + 32) != ss_rom_crc))) {
        f_close(&fss);
        DEBUG("savestates: header mismatch (flags %08x%08x vs %08x%08x, total %u vs %u)\n",
              (unsigned)(hflags >> 32), (unsigned)hflags,
              (unsigned)(ss_flags >> 32), (unsigned)ss_flags,
              (unsigned)total, (unsigned)ss_total_bytes);
        return SS_WRONG_GAME;
    }
    int blocks = (int)(total / 512);
    // Payload CRC over the whole file before the FPGA sees a byte.
    uint32_t pcrc = 0xFFFFFFFFu;
    for (int blk = 1; blk < blocks; blk++) {
        if (f_read(&fss, ss_io, 512, &br) != FR_OK || br != 512) {
            f_close(&fss);
            return SS_SD_ERROR;
        }
        pcrc = fast_crc32_update(pcrc, ss_io, 512);
        heartbeat_bump();
    }
    if ((pcrc ^ 0xFFFFFFFFu) != rd32(hdr + 36)) {
        f_close(&fss);
        return SS_CORRUPT;
    }
    if (f_lseek(&fss, 0) != FR_OK) {
        f_close(&fss);
        return SS_SD_ERROR;
    }

    SsStatus st;
    if (!ss_op_status(2, &st, kStatusTimeoutMs) || !st.ready) {
        ss_send_op(4);
        f_close(&fss);
        return SS_LINK_ERROR;
    }
    // Block 0 first, then let the core accept or reject it.
    if (f_read(&fss, ss_io, 512, &br) != FR_OK || br != 512)
        res = SS_SD_ERROR;
    else
        ss_send_block(0, ss_io);
    if (res == SS_OK && !ss_wait_state(&st, 5))
        res = st.error ? SS_WRONG_GAME : SS_LINK_ERROR;
    if (res == SS_OK)
        ss_last_disturbed = true;   // core was reset; payload decides the rest
    for (int blk = 1; blk < blocks && res == SS_OK; blk++) {
        if (f_read(&fss, ss_io, 512, &br) != FR_OK || br != 512) {
            res = SS_SD_ERROR;
            break;
        }
        ss_send_block(blk, ss_io);
        if ((blk & 31) == 31)
            overlay_status("Loading state %d/%dK", (blk * 512) >> 10,
                           (int)(total >> 10));
    }
    ss_send_op(res == SS_OK ? 3 : 4);
    f_close(&fss);
    heartbeat_bump();
    DEBUG("savestates: load slot %d -> %d\n", slot, (int)res);
    return res;
}

const char *savestate_result_str(savestate_result r) {
    switch (r) {
    case SS_OK: return "Done";
    case SS_UNSUPPORTED: return "Save states not supported here";
    case SS_NO_GAME: return "No game loaded";
    case SS_EMPTY: return "Slot is empty";
    case SS_WRONG_GAME: return "State is for another game/build";
    case SS_CORRUPT: return "State file corrupt";
    case SS_SD_ERROR: return "Card error (full?)";
    case SS_LINK_ERROR: return "Core did not answer";
    }
    return "Error";
}

// ---- RX hooks (UART RX task) ----

void savestates_rx_status(const uint8_t payload[16]) {
    memcpy(ss_rx_status, payload, 16);
    ss_rx_status_ok = 1;
    if (ss_sem) xSemaphoreGive(ss_sem);
}

void savestates_rx_byte(uint16_t blk, int idx, uint8_t b) {
    if ((int)blk != ss_expect_blk) return;
    if (idx >= 0 && idx < 512)
        ss_rx[idx] = b;
}

void savestates_rx_block_done(uint16_t blk) {
    ss_rx_block = blk;
    if (ss_sem) xSemaphoreGive(ss_sem);
}

void savestates_rx_bad_frame(void) {
    DEBUG("savestates: dropped a truncated 0x0F frame\n");
}
