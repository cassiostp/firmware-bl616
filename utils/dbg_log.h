// Debug log ring: timestamped lines kept in RAM for the USB debug console
// (usb/usb_console.cpp) and for the post-mortem dump to the SD card.
//
// dbg_log() is safe from any task, never blocks and never does I/O: it
// formats into a stack buffer and copies into the ring under a short
// critical section. It is NOT safe from interrupt handlers; ISRs keep cheap
// counters instead (see uart_rx_ring_dropped()) and a task logs the changes.
//
// The ring lives in the linker's .noinit_data section (bl616_flash.ld), so
// a software reset (MODE, watchdog) does not clear it. A magic + CRC header
// says whether the contents are a previous boot's log or power-on garbage;
// Sipeed's bootloader may still clobber the RAM, in which case the header
// check fails and nothing is dumped.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DBG_LOG_SIZE (12 * 1024)        // ring payload, bytes of log text
#define DBG_LOG_MAGIC 0x54434C47u       // "TCLG"

// Header stored in front of the ring data in .noinit_data. Exposed so the
// host tests can forge and corrupt headers.
struct dbg_pm_header {
    uint32_t magic;
    uint32_t size;          // DBG_LOG_SIZE this ring was built with
    uint32_t boot_count;    // incremented on every boot that finds a valid ring
    uint64_t total;         // bytes ever written (monotonic, survives resets)
    uint32_t fnv;           // FNV-1a over every byte ever written, in order
    uint32_t crc;           // CRC32 of the fields above (magic..fnv)
};

void dbg_log_init(void);                // once, early in main(), before other tasks log
void dbg_log(const char *fmt, ...);

// Free heap in bytes for log lines (mallinfo's fordblks), or -1 when this
// libc has no mallinfo (the SDK links heap_3, so there is no FreeRTOS
// heap query either). Safe from any task; never blocks.
int32_t dbg_log_heap_free(void);

// Stream/cursor access for the console task.
uint64_t dbg_log_total(void);           // bytes ever written
uint64_t dbg_log_dropped(void);         // bytes overwritten before anyone read them
// Copy bytes written since *cursor into dst (at most cap), oldest first, and
// advance *cursor past them. If the ring overwrote unread bytes, *cursor
// jumps to the oldest byte still held. Returns the byte count.
size_t dbg_log_read_since(uint64_t *cursor, char *dst, size_t cap);
// Visit every byte currently held, oldest first, in up to two chunks.
// Return false from cb to stop early.
void dbg_log_foreach(bool (*cb)(const char *data, size_t n, void *ctx), void *ctx);

// Post-mortem.
bool dbg_log_have_previous(void);       // a valid ring from an earlier boot exists
uint32_t dbg_log_boot_count(void);
bool dbg_pm_header_valid(const struct dbg_pm_header *h);
uint32_t dbg_crc32(const void *data, size_t n);

// Write the ring to a file. dump writes the whole ring to <dir>/lastlog.txt
// keeping the previous two as lastlog1.txt / lastlog2.txt; save writes
// <dir>/debuglog.txt, replacing it. dir includes the trailing slash and the
// drive, e.g. "sd:tangcore/". The caller makes sure the card is mounted and
// no save is in flight. Returns false on a file error.
bool dbg_log_dump_postmortem(const char *dir);
bool dbg_log_save(const char *dir);
bool dbg_log_save_as(const char *path);  // the same, to an exact file

// Diagnostic build: log one step of a game start, with the free heap and
// main_task's stack margin, and write the whole ring to
// <drive>tangcore/diaglog.txt at once. A hang right after a step still
// leaves that step on the card. Defined in main.cpp.
void diag_step(const char *fmt, ...);

#ifdef __cplusplus
}
#endif
