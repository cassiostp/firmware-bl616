// Unit tests for the debug log ring (utils/dbg_log.cpp), the command
// parser (utils/dbg_console.cpp) and the post-mortem dump. Runs on the
// host against stubs: a controllable clock, a mutex behind the critical
// sections and an in-memory FatFs. Run by host/run-tests.sh.
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "dbg_log.h"
#include "dbg_console.h"

extern "C" {
#include "ff.h"
}

/////////////////////////////////////////////////////////////////////////////////
// Stubs

static std::atomic<uint64_t> fake_ms{1000};
extern "C" uint64_t bflb_mtimer_get_time_ms(void) { return fake_ms.load(); }
extern "C" uint64_t bflb_mtimer_get_time_us(void) { return fake_ms.load() * 1000; }

static std::recursive_mutex crit_mu;
extern "C" void taskENTER_CRITICAL(void) { crit_mu.lock(); }
extern "C" void taskEXIT_CRITICAL(void) { crit_mu.unlock(); }

// In-memory FatFs: path -> contents. FIL carries the open path via _sim_fp.
static std::map<std::string, std::string> memfs;
extern "C" {
FRESULT f_open(FIL *fp, const char *path, BYTE mode) {
    if (mode & FA_CREATE_ALWAYS)
        memfs[path] = "";
    else if (!memfs.count(path))
        return FR_NO_FILE;
    fp->_sim_fp = (FILE *)new std::string(path);
    return FR_OK;
}
FRESULT f_write(FIL *fp, const void *buff, UINT btw, UINT *bw) {
    memfs[*(std::string *)fp->_sim_fp].append((const char *)buff, btw);
    *bw = btw;
    return FR_OK;
}
FRESULT f_close(FIL *fp) {
    delete (std::string *)fp->_sim_fp;
    fp->_sim_fp = nullptr;
    return FR_OK;
}
FRESULT f_unlink(const char *path) {
    return memfs.erase(path) ? FR_OK : FR_NO_FILE;
}
FRESULT f_rename(const char *old_path, const char *new_path) {
    auto it = memfs.find(old_path);
    if (it == memfs.end())
        return FR_NO_FILE;
    memfs[new_path] = it->second;
    memfs.erase(it);
    return FR_OK;
}
} // extern "C"

/////////////////////////////////////////////////////////////////////////////////
// Helpers

static int failures;
#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

static std::string ring_text() {
    std::string out;
    dbg_log_foreach([](const char *d, size_t n, void *ctx) {
        ((std::string *)ctx)->append(d, n);
        return true;
    }, &out);
    return out;
}

static std::string captured;            // console transport output
static bool transport_accept = true;
static bool fake_write(const char *data, size_t n) {
    if (!transport_accept)
        return false;
    captured.append(data, n);
    return true;
}

static int n_status, n_tasks, n_saves, n_link, n_reboot, n_wdtest;
static void cnt(dbg_emit_fn emit, int *c, const char *what) {
    (*c)++;
    emit("%s-body\n", what);
}
static const struct dbg_platform fake_platform = {
    [](dbg_emit_fn e) { cnt(e, &n_status, "status"); },
    [](dbg_emit_fn e) { cnt(e, &n_tasks, "tasks"); },
    [](dbg_emit_fn e) { cnt(e, &n_saves, "saves"); },
    [](dbg_emit_fn e) { cnt(e, &n_link, "link"); },
    []() { n_reboot++; },
    []() { n_wdtest++; },
};

int main() {
    // ---- ring basics + timestamps ----
    dbg_log_init();
    CHECK(!dbg_log_have_previous());
    CHECK(dbg_log_boot_count() == 1);
    fake_ms = 61234;
    dbg_log("hello %d", 42);
    std::string t = ring_text();
    CHECK(t.find("[61.234] hello 42\n") != std::string::npos);

    // ---- wrap: write well past the ring size ----
    for (int i = 0; i < 3000; i++)
        dbg_log("line %d padding padding padding", i);
    CHECK(dbg_log_total() > DBG_LOG_SIZE);
    CHECK(dbg_log_dropped() == dbg_log_total() - DBG_LOG_SIZE);
    t = ring_text();
    CHECK(t.size() == DBG_LOG_SIZE);
    CHECK(t.find("line 2999") != std::string::npos);
    CHECK(t.find("hello 42") == std::string::npos);      // long overwritten
    CHECK(t.back() == '\n');

    // ---- read_since cursor ----
    uint64_t cur = dbg_log_total();
    dbg_log("after-cursor");
    char buf[128];
    size_t n = dbg_log_read_since(&cur, buf, sizeof buf);
    CHECK(std::string(buf, n).find("after-cursor") != std::string::npos);
    CHECK(dbg_log_read_since(&cur, buf, sizeof buf) == 0);
    uint64_t stale = 0;                                   // overwritten cursor jumps
    n = dbg_log_read_since(&stale, buf, sizeof buf);
    CHECK(n > 0 && stale > 0);

    // ---- second boot sees the previous ring ----
    dbg_log("pre-reset-marker");
    dbg_log_init();                                       // simulated software reset
    CHECK(dbg_log_have_previous());
    CHECK(dbg_log_boot_count() == 2);
    CHECK(ring_text().find("pre-reset-marker") != std::string::npos);
    CHECK(ring_text().find("previous log above") != std::string::npos);

    // ---- header validation (forge + corrupt) ----
    struct dbg_pm_header h = {};
    h.magic = DBG_LOG_MAGIC;
    h.size = DBG_LOG_SIZE;
    h.boot_count = 7;
    h.crc = dbg_crc32(&h, offsetof(struct dbg_pm_header, crc));
    CHECK(dbg_pm_header_valid(&h));
    h.boot_count = 8;                                     // any field change breaks CRC
    CHECK(!dbg_pm_header_valid(&h));
    h.boot_count = 7;
    h.magic = 0;
    CHECK(!dbg_pm_header_valid(&h));

    // ---- concurrent writers never lose the ring's structure ----
    {
        std::vector<std::thread> th;
        for (int w = 0; w < 4; w++)
            th.emplace_back([w] {
                for (int i = 0; i < 500; i++)
                    dbg_log("writer %d line %d", w, i);
            });
        for (auto &x : th)
            x.join();
        std::string all = ring_text();
        CHECK(all.size() == DBG_LOG_SIZE);
        // every line ends with \n even under contention (critical section)
        CHECK(all.back() == '\n');
        CHECK(all.find("writer 3 line 499") != std::string::npos);
    }

    // ---- post-mortem dump + rotation ----
    dbg_log("dump-marker");
    CHECK(dbg_log_dump_postmortem("sd:tangcore/"));
    CHECK(memfs["sd:tangcore/lastlog.txt"].find("dump-marker") != std::string::npos);
    CHECK(dbg_log_dump_postmortem("sd:tangcore/"));
    CHECK(memfs.count("sd:tangcore/lastlog1.txt") == 1);
    CHECK(dbg_log_save("sd:tangcore/"));
    CHECK(memfs["sd:tangcore/debuglog.txt"].find("dump-marker") != std::string::npos);

    // ---- command parser ----
    dbg_console_init(&fake_platform);
    dbg_console_set_transport(fake_write);
    dbg_console_set_active(true);
    captured.clear();

    dbg_console_feed("help\n", 5);
    CHECK(captured.find("status") != std::string::npos);
    captured.clear();
    dbg_console_feed("status\r\n", 8);
    CHECK(n_status == 1 && captured.find("status-body") != std::string::npos);
    dbg_console_feed("tasks\nsaves\nlink\n", 17);
    CHECK(n_tasks == 1 && n_saves == 1 && n_link == 1);

    // backspace editing: "statx<BS>us" -> "status"
    dbg_console_feed("statx", 5);
    dbg_console_feed("\x7f", 1);
    dbg_console_feed("us\n", 3);
    CHECK(n_status == 2);

    // unknown command is answered, not fatal
    captured.clear();
    dbg_console_feed("frobnicate\n", 11);
    CHECK(captured.find("unknown command") != std::string::npos);

    // log dump contains ring text
    captured.clear();
    dbg_console_feed("log\n", 4);
    CHECK(captured.find("writer") != std::string::npos);

    // wdtest needs the confirmation dance
    dbg_console_feed("wdtest\n", 7);
    CHECK(n_wdtest == 0);
    dbg_console_feed("wdtest yes\n", 11);
    CHECK(n_wdtest == 1);
    // ... and the confirmation expires
    dbg_console_feed("wdtest\n", 7);
    fake_ms += 20000;
    dbg_console_feed("wdtest yes\n", 11);
    CHECK(n_wdtest == 1);

    // sniff toggles and reports counts once a second
    captured.clear();
    dbg_console_feed("sniff on\n", 9);
    dbg_sniff_note_tx(0x03);
    dbg_sniff_note_tx(0x03);
    dbg_sniff_note_rx(0x03);
    fake_ms += 1100;
    dbg_console_poll();
    CHECK(captured.find("sniff 03:tx2/rx1") != std::string::npos);
    dbg_console_feed("sniff off\n", 10);

    // reboot goes to the platform
    dbg_console_feed("reboot\n", 7);
    CHECK(n_reboot == 1);

    // ---- never blocks when the PC stops reading ----
    transport_accept = false;
    uint32_t before = dbg_console_dropped();
    for (int i = 0; i < 200; i++) {
        dbg_log("unread %d", i);
        dbg_console_poll();                               // must return, dropping
    }
    CHECK(dbg_console_dropped() > before);
    transport_accept = true;

    // live streaming resumes for a reader
    dbg_console_poll();                                   // flush backlog state
    captured.clear();
    dbg_log("streamed-live");
    dbg_console_poll();
    CHECK(captured.find("streamed-live") != std::string::npos);

    if (failures) {
        printf("dbg_log_test: %d FAILURES\n", failures);
        return 1;
    }
    printf("dbg_log_test: all checks passed\n");
    return 0;
}
