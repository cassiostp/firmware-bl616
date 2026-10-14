#include "dbg_log.h"

#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifndef TANGCORE_HOST
// newlib may declare mallinfo without providing it (heap_3): weak, so the
// null check in dbg_log_heap_free() says whether it exists.
extern "C" __attribute__((weak)) struct mallinfo mallinfo(void);
#endif

extern "C" {
#include <FreeRTOS.h>
#include "task.h"
#include "bflb_mtimer.h"
#include "ff.h"
}

// The whole ring (header + data) sits in .noinit_data: the C runtime zeroes
// .bss only, so the contents survive a software reset.
struct dbg_ring {
    struct dbg_pm_header hdr;
    char data[DBG_LOG_SIZE];
};

#ifndef TANGCORE_HOST
__attribute__((section(".noinit_data"))) static struct dbg_ring ring;
#else
static struct dbg_ring ring;            // host sim/tests: plain RAM
#endif

static bool have_previous;

uint32_t dbg_crc32(const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1));
    }
    return crc ^ 0xFFFFFFFFu;
}

static uint32_t header_crc(const struct dbg_pm_header *h) {
    return dbg_crc32(h, offsetof(struct dbg_pm_header, crc));
}

bool dbg_pm_header_valid(const struct dbg_pm_header *h) {
    if (h->magic != DBG_LOG_MAGIC || h->size != DBG_LOG_SIZE)
        return false;
    return h->crc == header_crc(h);
}

// Recompute the rolling hash over the bytes the ring still holds. The hash
// in the header covers every byte ever written, so it only matches while
// total <= DBG_LOG_SIZE; after the first wrap it can no longer be checked
// this way and the header CRC + magic are the evidence. (A clobbered RAM
// would have to forge both.) Data bytes themselves are text: a partly
// clobbered tail only garbles the dump, it cannot crash the writer.
static bool data_plausible(void) {
    if (ring.hdr.total > DBG_LOG_SIZE)
        return true;
    uint32_t fnv = 2166136261u;
    for (uint64_t i = 0; i < ring.hdr.total; i++) {
        fnv ^= (uint8_t)ring.data[i];
        fnv *= 16777619u;
    }
    return fnv == ring.hdr.fnv;
}

void dbg_log_init(void) {
    have_previous = dbg_pm_header_valid(&ring.hdr) && data_plausible();
    if (!have_previous) {
        memset(&ring, 0, sizeof ring);
        ring.hdr.magic = DBG_LOG_MAGIC;
        ring.hdr.size = DBG_LOG_SIZE;
        ring.hdr.boot_count = 0;
        ring.hdr.total = 0;
        ring.hdr.fnv = 2166136261u;
    }
    ring.hdr.boot_count++;
    ring.hdr.crc = header_crc(&ring.hdr);
    if (have_previous)
        dbg_log("--- boot %lu, previous log above ---", (unsigned long)ring.hdr.boot_count);
}

static void ring_put(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        ring.data[ring.hdr.total % DBG_LOG_SIZE] = s[i];
        ring.hdr.fnv ^= (uint8_t)s[i];
        ring.hdr.fnv *= 16777619u;
        ring.hdr.total++;
    }
    ring.hdr.crc = header_crc(&ring.hdr);
}

void dbg_log(const char *fmt, ...) {
    char buf[200];
    uint64_t ms = bflb_mtimer_get_time_ms();
    int head = snprintf(buf, sizeof buf, "[%lu.%03lu] ",
                        (unsigned long)(ms / 1000), (unsigned long)(ms % 1000));
    if (head < 0)
        head = 0;
    va_list ap;
    va_start(ap, fmt);
    int body = vsnprintf(buf + head, sizeof buf - (size_t)head, fmt, ap);
    va_end(ap);
    size_t len = (size_t)head + (body > 0 ? (size_t)body : 0);
    if (len >= sizeof buf)
        len = sizeof buf - 1;
    if (len == 0 || buf[len - 1] != '\n') {
        if (len < sizeof buf - 1)
            buf[len++] = '\n';
        else
            buf[len - 1] = '\n';
    }
    taskENTER_CRITICAL();
    ring_put(buf, len);
    taskEXIT_CRITICAL();
}

int32_t dbg_log_heap_free(void) {
#ifdef TANGCORE_HOST
    return (int32_t)mallinfo2().fordblks;   // glibc: mallinfo() is deprecated
#else
    if (&mallinfo)
        return (int32_t)mallinfo().fordblks;
    return -1;
#endif
}

uint64_t dbg_log_total(void) {
    return ring.hdr.total;
}

uint64_t dbg_log_dropped(void) {
    return ring.hdr.total > DBG_LOG_SIZE ? ring.hdr.total - DBG_LOG_SIZE : 0;
}

bool dbg_log_have_previous(void) {
    return have_previous;
}

uint32_t dbg_log_boot_count(void) {
    return ring.hdr.boot_count;
}

// Oldest byte position still held.
static uint64_t ring_oldest(void) {
    return ring.hdr.total > DBG_LOG_SIZE ? ring.hdr.total - DBG_LOG_SIZE : 0;
}

size_t dbg_log_read_since(uint64_t *cursor, char *dst, size_t cap) {
    size_t out = 0;
    taskENTER_CRITICAL();
    uint64_t oldest = ring_oldest();
    if (*cursor < oldest)
        *cursor = oldest;
    while (*cursor < ring.hdr.total && out < cap) {
        dst[out++] = ring.data[*cursor % DBG_LOG_SIZE];
        (*cursor)++;
    }
    taskEXIT_CRITICAL();
    return out;
}

void dbg_log_foreach(bool (*cb)(const char *data, size_t n, void *ctx), void *ctx) {
    // Snapshot the range under the lock, then walk it unlocked: a writer
    // wrapping past us mid-walk only garbles text.
    taskENTER_CRITICAL();
    uint64_t start = ring_oldest();
    uint64_t end = ring.hdr.total;
    taskEXIT_CRITICAL();
    uint64_t pos = start;
    while (pos < end) {
        size_t off = (size_t)(pos % DBG_LOG_SIZE);
        size_t chunk = DBG_LOG_SIZE - off;
        if (chunk > end - pos)
            chunk = (size_t)(end - pos);
        if (!cb(ring.data + off, chunk, ctx))
            return;
        pos += chunk;
    }
}

/////////////////////////////////////////////////////////////////////////////////
// Dumping to the card

struct file_ctx {
    FIL *f;
    bool ok;
};

static bool write_chunk(const char *data, size_t n, void *ctx_) {
    struct file_ctx *ctx = (struct file_ctx *)ctx_;
    UINT bw = 0;
    if (f_write(ctx->f, data, (UINT)n, &bw) != FR_OK || bw != n) {
        ctx->ok = false;
        return false;
    }
    return true;
}

static bool write_ring_to(const char *path) {
    FIL f;
    if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
        return false;
    struct file_ctx ctx = { &f, true };
    dbg_log_foreach(write_chunk, &ctx);
    bool ok = ctx.ok && f_close(&f) == FR_OK;
    if (!ctx.ok)
        f_close(&f);
    return ok;
}

static void rename_over(const char *from, const char *to) {
    f_unlink(to);
    f_rename(from, to);
}

bool dbg_log_dump_postmortem(const char *dir) {
    char newest[64], older[64], oldest[64];
    snprintf(newest, sizeof newest, "%slastlog.txt", dir);
    snprintf(older, sizeof older, "%slastlog1.txt", dir);
    snprintf(oldest, sizeof oldest, "%slastlog2.txt", dir);
    rename_over(older, oldest);
    rename_over(newest, older);
    return write_ring_to(newest);
}

bool dbg_log_save(const char *dir) {
    char path[64];
    snprintf(path, sizeof path, "%sdebuglog.txt", dir);
    return write_ring_to(path);
}
