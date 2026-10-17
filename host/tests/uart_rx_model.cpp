// Why the UART receive path is interrupt-driven (utils/uart_rx.cpp).
//
// A model of the BL616 UART1 receive side: the FPGA's frames arrive back to
// back at 2 Mbaud (one byte per 5 us), into a 32-byte hardware FIFO that drops
// what does not fit. Two firmware designs read it:
//
//   POLL   the old uart1_rx_task: drain the FIFO, then vTaskDelay(1) (a tick,
//          1 ms). Reading a byte costs ~1 us, so the task always catches up
//          with the line, sleeps, and the FIFO overflows while it does.
//   IRQ    the firmware's real uart_rx_isr and ring buffer, driven by the
//          modelled FIFO (interrupt at 8 bytes, or when the line goes quiet),
//          with a task that reads the ring once a tick.
//
// A battery-save block is a 515-byte frame (518 bytes with the 3 header bytes). The test sends several and checks
// what arrives. It links the REAL ring and ISR from utils/uart_rx.cpp; only the
// four UART register reads are modelled.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "uart_rx.h"
#include "utils.h"

extern "C" void uart_rx_isr(int irq, void *arg);

// ---- the modelled hardware -------------------------------------------------
namespace {

const int FIFO_DEPTH = 32;
const int BYTE_US = 5;              // 10 bits at 2 Mbaud
const int FIFO_THRESHOLD = 8;       // rx_fifo_threshold 7: interrupt above 7 bytes
const int RTO_US = 40;              // receiver timeout: quiet this long with bytes waiting

struct Hw {
    std::vector<uint8_t> wire;      // bytes the FPGA sends, one per BYTE_US
    int64_t wire_start = 0;
    size_t next_wire = 0;
    int64_t now = 0;                // us
    uint8_t fifo[FIFO_DEPTH];
    int count = 0;
    int head = 0;
    int lost = 0;
    int64_t last_arrival = -1;
    bool isr_pending = false;       // interrupt asserted, waiting for the CPU
    int64_t isr_since = 0;

    void arrivals_until(int64_t t) {
        while (next_wire < wire.size() && wire_start + (int64_t)(next_wire + 1) * BYTE_US <= t) {
            int64_t at = wire_start + (int64_t)(next_wire + 1) * BYTE_US;
            if (count == FIFO_DEPTH) {
                lost++;
            } else {
                fifo[(head + count) % FIFO_DEPTH] = wire[next_wire];
                count++;
            }
            next_wire++;
            last_arrival = at;
            if (count >= FIFO_THRESHOLD && !isr_pending) {
                isr_pending = true;
                isr_since = at;
            }
        }
        if (count > 0 && last_arrival >= 0 && t - last_arrival >= RTO_US && !isr_pending) {
            isr_pending = true;     // receiver timeout
            isr_since = last_arrival + RTO_US;
        }
    }
    void advance(int64_t dt) {
        now += dt;
        arrivals_until(now);
    }
};
Hw hw;

} // namespace

// The register-level calls utils/uart_rx.cpp makes. Each read costs the CPU
// 1 us, during which the line keeps delivering.
struct bflb_device_s g_uart_dev;
struct bflb_device_s *uart1_dev = &g_uart_dev;

extern "C" {
int bflb_uart_rxavailable(struct bflb_device_s *) {
    return hw.count > 0;
}
uint8_t bflb_uart_getchar(struct bflb_device_s *) {
    uint8_t b = hw.fifo[hw.head];
    hw.head = (hw.head + 1) % FIFO_DEPTH;
    hw.count--;
    hw.advance(1);
    return b;
}
uint32_t bflb_uart_get_intstatus(struct bflb_device_s *) {
    return 0;
}
void bflb_uart_int_clear(struct bflb_device_s *, uint32_t) {}
void bflb_uart_rxint_mask(struct bflb_device_s *, bool) {}
void bflb_irq_attach(int, void (*)(int, void *), void *) {}
void bflb_irq_enable(int) {}
}

// ---- the scenario ------------------------------------------------------------
namespace {

// n frames of 515 bytes: AA 02 03 0A blk_hi blk_lo, 512 data bytes... as the
// FPGA's save block, the data a counting pattern (so a lost byte shows).
std::vector<uint8_t> make_wire(int frames) {
    std::vector<uint8_t> w;
    for (int f = 0; f < frames; f++) {
        w.push_back(0xAA);
        w.push_back(0x02);
        w.push_back(0x03);
        w.push_back(0x0A);
        w.push_back(0);
        w.push_back((uint8_t)f);
        for (int i = 0; i < 512; i++)
            w.push_back((uint8_t)(i * 7 + f));
    }
    return w;
}

void start(const std::vector<uint8_t> &wire, int64_t phase_us) {
    hw = Hw();
    hw.wire = wire;
    hw.wire_start = phase_us;       // the line is idle until the first byte
    uart_rx_ring_reset();
}

// The old receive task: drain, then sleep to the next 1 ms tick.
// Returns the bytes it saw, in order.
std::vector<uint8_t> run_poll(const std::vector<uint8_t> &wire, int64_t phase_us) {
    start(wire, phase_us);
    std::vector<uint8_t> got;
    int64_t end = phase_us + (int64_t)wire.size() * BYTE_US + 3000;
    while (hw.now < end) {
        hw.arrivals_until(hw.now);
        while (hw.count > 0) {
            got.push_back(bflb_uart_getchar(nullptr));
            hw.advance(0);
        }
        // vTaskDelay(1): the next tick boundary (the task runs on ticks)
        int64_t next = (hw.now / 1000 + 1) * 1000;
        hw.advance(next - hw.now);
    }
    return got;
}

// The new design. latency_us: how long the interrupt waits for the CPU
// (another interrupt or a critical section holding it off).
std::vector<uint8_t> run_irq(const std::vector<uint8_t> &wire, int64_t phase_us, int latency_us) {
    start(wire, phase_us);
    std::vector<uint8_t> got;
    int64_t end = phase_us + (int64_t)wire.size() * BYTE_US + 3000;
    int64_t next_tick = 1000;
    while (hw.now < end) {
        hw.advance(1);
        if (hw.isr_pending && hw.now - hw.isr_since >= latency_us) {
            hw.isr_pending = false;
            uart_rx_isr(0, nullptr);        // the real handler: drains into the ring
            hw.arrivals_until(hw.now);
        }
        if (hw.now >= next_tick) {          // the task, once a tick
            uint8_t b;
            while (uart_rx_get(&b))
                got.push_back(b);
            next_tick += 1000;
        }
    }
    uint8_t b;
    while (uart_rx_get(&b))
        got.push_back(b);
    return got;
}

int g_fail = 0;
void check(bool ok, const char *what) {
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        g_fail++;
}

bool same(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b) {
    return a.size() == b.size() && memcmp(a.data(), b.data(), a.size()) == 0;
}

} // namespace

int main() {
    const std::vector<uint8_t> one = make_wire(1);
    const std::vector<uint8_t> four = make_wire(4);
    printf("uart rx model: 515-byte save blocks at 2 Mbaud into a 32-byte FIFO\n");

    printf("old design (poll, sleep 1 ms):\n");
    int polled_whole = 0, phases = 0;
    size_t min_got = one.size();
    for (int phase = 0; phase < 1000; phase += 50) {
        auto got = run_poll(one, phase);
        phases++;
        if (same(got, one))
            polled_whole++;
        if (got.size() < min_got)
            min_got = got.size();
    }
    printf("  a lone block arrived whole in %d of %d start phases; worst case %zu of %zu bytes\n",
           polled_whole, phases, min_got, one.size());
    check(polled_whole == 0, "polling never receives a whole block");

    printf("new design (interrupt drains the FIFO into the ring, task reads once a tick):\n");
    bool all = true;
    for (int phase = 0; phase < 1000; phase += 50)
        all = all && same(run_irq(one, phase, 20), one);
    check(all, "a lone block arrives whole at every start phase (20 us interrupt latency)");

    check(same(run_irq(four, 123, 20), four), "four blocks back to back arrive whole and in order");
    check(same(run_irq(four, 123, 100), four), "100 us interrupt latency is fine");
    check(same(run_irq(four, 123, 110), four),
          "110 us latency is fine (the interrupt fires at 8 bytes; 24 more fit in 120 us)");
    auto late = run_irq(four, 123, 300);
    check(late.size() < four.size() && hw.lost > 0,
          "300 us of blocked interrupts overflows the FIFO (the limit this design has)");

    printf(g_fail ? "FAILED (%d)\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
