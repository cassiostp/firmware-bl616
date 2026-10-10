// Virtual simulation clock. See sim_time.hpp for the design.
#include "sim_time.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace sim {

namespace {

// FIFO-fair mutex for the scheduler lock. std::mutex has no fairness: a
// thread stepping advance chunks in a tight unlock/yield/relock loop can keep
// re-acquiring ahead of a woken waiter indefinitely (the waiter needs
// microseconds to wake while the spinner re-locks in nanoseconds), stalling
// the waiter's sim-time waits for a whole polling window. A ticket lock hands
// off in FIFO order, so chunk steps interleave fairly among drivers and every
// waiter observes its deadline within (number of contenders) chunks. Equality
// comparison makes the 32-bit wrap benign (never more than a handful of live
// tickets).
class TicketMutex {
  public:
    void lock() {
        unsigned t = next_.fetch_add(1, std::memory_order_relaxed);
        unsigned spins = 0;
        while (now_.load(std::memory_order_acquire) != t) {
            // The owner usually finishes its chunk in microseconds; yield
            // briefly, then sleep so a long (RTL model stepping) chunk does
            // not burn a core.
            if (++spins < 50)
                std::this_thread::yield();
            else
                std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
    void unlock() {
        now_.fetch_add(1, std::memory_order_release);
    }

  private:
    std::atomic<unsigned> next_{0};
    std::atomic<unsigned> now_{0};
};

TicketMutex g_m;
std::condition_variable_any g_cv;
// The clock itself. Atomic so observing time (now_ticks) never contends with
// stepping: the firmware reads the clock constantly (per UART byte in the RX
// task's resync check, per menu poll), and routing every read through the
// scheduler lock made a 519-byte save-block drain cost ~519 lock turns --
// past the firmware's 1 s block timeout whenever several threads drove at
// once. Only stepping (advance_to/wait_until, under g_m) writes it.
std::atomic<uint64_t> g_ticks{0};
Backend *g_backend = nullptr;

struct NullBackend : Backend {
    void run_until(uint64_t) override {}
};
NullBackend g_null;

// Rate cap. Every advance chunk (5 sim-ms) sleeps SLEEP_PER_CHUNK of wall
// time. Without it the sim runs 30-3000 sim-s per wall-s (faster whenever
// fewer threads contend), and firmware timeouts -- the 1 s save-block fetch,
// the 2 s save debounce, the 200 ms core-ID poll -- translate to 0.3-30 ms of
// wall time, well inside normal CFS scheduling jitter (a thread routinely
// waits 10-50 ms for a CPU). The result is spurious timeouts with no retry:
// a single slow block fetch fails the whole save dump. The cap keeps the
// combined rate near ~10 sim-s per wall-s with several drivers, so a 1 s
// timeout means ~100 ms of wall time and every dependent thread (the RX task
// draining the reply, the save task observing its deadline) gets scheduled
// many times over inside the window. It also paces scripted presses: the
// firmware's post-action debounce then takes milliseconds of wall time, so
// the script always observes the OSD reaction and releases before the menu
// can sample the still-held level a second time (this replaces press_stretch).
void yield_wall(int chunks) {
    (void)chunks;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
}

} // namespace

void set_backend(Backend *b) {
    std::lock_guard<TicketMutex> lk(g_m);
    g_backend = b ? b : (Backend *)&g_null;
}

void kick() {
    std::lock_guard<TicketMutex> lk(g_m);
    g_cv.notify_all();
}

uint64_t now_ticks() {
    return g_ticks.load(std::memory_order_relaxed);
}

void advance_to(uint64_t target) {
    std::unique_lock<TicketMutex> lk(g_m);
    if (!g_backend)
        g_backend = &g_null;
    int chunks = 0;
    while (g_ticks.load(std::memory_order_relaxed) < target) {
        uint64_t step = std::min(target, g_ticks.load(std::memory_order_relaxed) + STEP_CHUNK);
        g_backend->run_until(step);
        g_ticks.store(step, std::memory_order_relaxed);
        g_cv.notify_all();
        lk.unlock();
        yield_wall(chunks++);
        lk.lock();
    }
}

bool wait_until(uint64_t deadline, const std::function<bool()> &pred) {
    // pred must be callable with no locks held (it takes leaf locks itself):
    // g_m is always dropped around the check, so a giver holding a leaf
    // lock and kicking (t.m -> g_m) can never deadlock against us (g_m ->
    // t.m is never nested).
    std::unique_lock<TicketMutex> lk(g_m);
    if (!g_backend)
        g_backend = &g_null;
    if (deadline == INF) {
        // Pure event wait: someone else drives the clock.
        for (;;) {
            lk.unlock();
            bool r = pred();
            lk.lock();
            if (r)
                return true;
            g_cv.wait(lk);
        }
    }
    int chunks = 0;
    bool drove = false;
    for (;;) {
        lk.unlock();
        bool r = pred();
        lk.lock();
        if (r) {
            return true;
        }
        if (g_ticks.load(std::memory_order_relaxed) >= deadline)
            break;
        uint64_t step =
            std::min(deadline, g_ticks.load(std::memory_order_relaxed) + STEP_CHUNK);
        g_backend->run_until(step);
        g_ticks.store(step, std::memory_order_relaxed);
        drove = true;
        g_cv.notify_all();
        lk.unlock();
        yield_wall(chunks++);
        lk.lock();
    }
    if (!drove) {
        // We never moved the clock (zero timeout, or already expired): no
        // thread can be landing the event on our behalf. Report at once.
        lk.unlock();
        return pred();
    }
    // Expired, but threads woken by our own advance may still be landing the
    // event (e.g. the RX task draining a byte our jump produced). Give them
    // a short wall-clock grace before reporting the timeout (20 ms: at the
    // capped rate the woken threads need a few wall-ms to be scheduled).
    lk.unlock();
    auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        if (pred())
            return true;
        if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(20))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

} // namespace sim
