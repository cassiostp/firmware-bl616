#include "dbg_console.h"
#include "dbg_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

extern "C" {
#include "bflb_mtimer.h"
}

#define LINE_LEN 128
#define CONFIRM_MS 10000        // "wdtest yes" must follow within this

static const struct dbg_platform *platform;
static bool (*transport_write)(const char *data, size_t n);
static bool active;
static uint32_t dropped_bytes;
static uint64_t stream_cursor;          // dbg_log position already streamed
static char line[LINE_LEN];
static size_t line_len;
static bool sniff_on;
static uint64_t sniff_last_ms;
static uint64_t wdtest_asked_ms;        // 0 = not asked
static bool last_feed_cr;               // previous feed ended on '\r'

// Sniff counters, indexed by frame type (0..31 kept, the rest in bucket 31).
static volatile uint32_t sniff_tx[32];
static volatile uint32_t sniff_rx[32];
static uint32_t sniff_tx_seen[32];
static uint32_t sniff_rx_seen[32];

void dbg_sniff_note_tx(uint8_t cmd) {
    sniff_tx[cmd < 31 ? cmd : 31]++;
}

void dbg_sniff_note_rx(uint8_t type) {
    sniff_rx[type < 31 ? type : 31]++;
}

static void emit_raw(const char *data, size_t n) {
    if (!transport_write || !transport_write(data, n))
        dropped_bytes += (uint32_t)n;
}

static void emit(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    size_t len = (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1;
    // Terminals want CRLF; the log ring stores LF only. Convert here.
    size_t out = 0;
    char crlf[512];
    for (size_t i = 0; i < len && out + 2 < sizeof crlf; i++) {
        if (buf[i] == '\n')
            crlf[out++] = '\r';
        crlf[out++] = buf[i];
    }
    emit_raw(crlf, out);
}

static void prompt(void) {
    emit_raw("tc> ", 4);
}

void dbg_console_init(const struct dbg_platform *p) {
    platform = p;
    stream_cursor = dbg_log_total();
}

void dbg_console_set_transport(bool (*write)(const char *data, size_t n)) {
    transport_write = write;
}

void dbg_console_set_active(bool on) {
    active = on;
    if (on) {
        stream_cursor = dbg_log_total();    // don't replay history; `log` does that
        line_len = 0;
        wdtest_asked_ms = 0;
        emit("\nTangCore debug console. 'help' lists commands.\n");
        prompt();
    }
}

bool dbg_console_active(void) {
    return active;
}

uint32_t dbg_console_dropped(void) {
    return dropped_bytes;
}

static bool dump_chunk(const char *data, size_t n, void *ctx) {
    (void)ctx;
    // emit() handles the CRLF conversion; hand it buffer-sized pieces.
    size_t i = 0;
    while (i < n) {
        char piece[256];
        size_t plen = n - i;
        if (plen > sizeof piece - 1)
            plen = sizeof piece - 1;
        memcpy(piece, data + i, plen);
        piece[plen] = 0;
        emit("%s", piece);
        i += plen;
    }
    return true;
}

static void run_command(char *cmd) {
    // Trim trailing spaces; split off one argument.
    char *arg = cmd;
    while (*arg && *arg != ' ')
        arg++;
    if (*arg) {
        *arg++ = 0;
        while (*arg == ' ')
            arg++;
    }

    if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) {
        emit("help          this list\n"
             "status        diag rows, core, uptime, heap\n"
             "tasks         task states and stack high-water marks\n"
             "log           dump the log ring\n"
             "saves         battery-save counters\n"
             "link          FPGA UART link counters\n"
             "sniff on|off  per-second MCU<->FPGA frame counts\n"
             "wdtest        stall the watchdog on purpose (asks first)\n"
             "reboot        restart the MCU\n");
    } else if (!strcmp(cmd, "status")) {
        if (platform && platform->cmd_status)
            platform->cmd_status(emit);
        else
            emit("status unavailable\n");
        emit("console: dropped %lu output bytes, log overwrote %llu bytes\n",
             (unsigned long)dropped_bytes, (unsigned long long)dbg_log_dropped());
    } else if (!strcmp(cmd, "tasks")) {
        if (platform && platform->cmd_tasks)
            platform->cmd_tasks(emit);
        else
            emit("tasks unavailable\n");
    } else if (!strcmp(cmd, "log")) {
        uint64_t end = dbg_log_total();     // lines after this still stream
        dbg_log_foreach(dump_chunk, NULL);
        stream_cursor = end;
    } else if (!strcmp(cmd, "saves")) {
        if (platform && platform->cmd_saves)
            platform->cmd_saves(emit);
        else
            emit("saves unavailable\n");
    } else if (!strcmp(cmd, "link")) {
        if (platform && platform->cmd_link)
            platform->cmd_link(emit);
        else
            emit("link unavailable\n");
    } else if (!strcmp(cmd, "sniff")) {
        if (!strcmp(arg, "on")) {
            sniff_on = true;
            memcpy(sniff_tx_seen, (const void *)sniff_tx, sizeof sniff_tx_seen);
            memcpy(sniff_rx_seen, (const void *)sniff_rx, sizeof sniff_rx_seen);
            sniff_last_ms = bflb_mtimer_get_time_ms();
            emit("sniff on: one summary line per second\n");
        } else if (!strcmp(arg, "off")) {
            sniff_on = false;
            emit("sniff off\n");
        } else {
            emit("usage: sniff on|off (now %s)\n", sniff_on ? "on" : "off");
        }
    } else if (!strcmp(cmd, "wdtest")) {
        uint64_t now = bflb_mtimer_get_time_ms();
        if (!strcmp(arg, "yes") && wdtest_asked_ms && now - wdtest_asked_ms < CONFIRM_MS) {
            wdtest_asked_ms = 0;
            dbg_log("wdtest: watchdog stall requested from console");
            emit("wdtest: heartbeat stalled; the firmware restarts itself in ~15 s.\n"
                 "Expect the menu back within ~20 s, with or without the PC attached.\n");
            if (platform && platform->do_wdtest)
                platform->do_wdtest();
        } else {
            wdtest_asked_ms = now;
            emit("wdtest stalls the heartbeat so the hardware watchdog fires.\n"
                 "Type 'wdtest yes' within 10 s to confirm.\n");
        }
    } else if (!strcmp(cmd, "reboot")) {
        dbg_log("reboot requested from console");
        emit("rebooting (unplug the PC first, or the board starts in debug mode)\n");
        if (platform && platform->do_reboot)
            platform->do_reboot();
        emit("reboot unavailable\n");
    } else if (cmd[0]) {
        emit("unknown command '%s' (try help)\n", cmd);
    }
    prompt();
}

void dbg_console_feed(const char *data, size_t n) {
    if (!active)
        return;
    for (size_t i = 0; i < n; i++) {
        char c = data[i];
        if (c == '\r' || c == '\n') {
            bool was_cr = (i > 0 && data[i - 1] == '\r') || (i == 0 && last_feed_cr);
            if (c == '\n' && line_len == 0 && was_cr) {
                last_feed_cr = false;
                continue;                   // second half of CRLF after Enter
            }
            emit_raw("\r\n", 2);
            line[line_len] = 0;
            run_command(line);
            line_len = 0;
        } else if (c == '\b' || c == 0x7f) {
            if (line_len) {
                line_len--;
                emit_raw("\b \b", 3);
            }
        } else if (c >= ' ' && c < 0x7f) {
            last_feed_cr = false;
            if (line_len < LINE_LEN - 1) {
                line[line_len++] = c;
                emit_raw(&c, 1);            // echo
            }
        }
    }
    if (n)
        last_feed_cr = data[n - 1] == '\r';
}

void dbg_console_poll(void) {
    if (!active)
        return;

    // Stream new log lines. If the user is mid-line this interleaves with
    // their typing on screen; acceptable for a debug console, and their
    // input buffer here is unaffected.
    char buf[256];
    size_t n;
    while ((n = dbg_log_read_since(&stream_cursor, buf, sizeof buf - 1)) > 0) {
        buf[n] = 0;
        emit("%s", buf);
        if (n < sizeof buf - 1)
            break;
    }

    uint64_t now = bflb_mtimer_get_time_ms();
    if (sniff_on && now - sniff_last_ms >= 1000) {
        sniff_last_ms = now;
        char parts[200];
        size_t off = 0;
        bool any = false;
        for (int t = 0; t < 32; t++) {
            uint32_t tx = sniff_tx[t] - sniff_tx_seen[t];
            uint32_t rx = sniff_rx[t] - sniff_rx_seen[t];
            sniff_tx_seen[t] = sniff_tx[t];
            sniff_rx_seen[t] = sniff_rx[t];
            if ((tx || rx) && off + 24 < sizeof parts) {
                off += (size_t)snprintf(parts + off, sizeof parts - off,
                                         " %02x:tx%lu/rx%lu", t,
                                         (unsigned long)tx, (unsigned long)rx);
                any = true;
            }
        }
        emit("sniff%s\n", any ? parts : " (idle)");
    }

    if (wdtest_asked_ms && now - wdtest_asked_ms >= CONFIRM_MS)
        wdtest_asked_ms = 0;
}
