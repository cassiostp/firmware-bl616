// Virtual simulation clock for tangcore-sim.
//
// Deterministic discrete-event scheduling: the clock advances ONLY when
// every participating thread is blocked (in a sim wait, on a sim mutex, or
// parked). A dedicated scheduler thread then steps the clock to the
// earliest pending deadline (in chunks of at most STEP_CHUNK, stepping the
// backend model), wakes the threads that are due, and waits for them all
// to block again before the next step. While any thread is runnable the
// clock is frozen, so a thread can never be "timed out" by other threads
// driving time past its deadline while the host has not scheduled it yet.
// Test outcomes therefore do not depend on host speed or core count: a
// slow, loaded runner only makes the same sequence of steps take longer
// in wall time.
//
// The previous design let every vTaskDelay/wait drive the shared clock
// forward itself (rate-capped with a wall sleep per chunk). On a loaded
// few-core CI runner a runnable task (e.g. the UART RX task delivering a
// core-ID reply) routinely waited longer for a CPU than firmware timeouts
// translated to in wall time (30 ms MODE-poll timeout ~ a few wall-ms),
// so polls spuriously missed, silence windows were jumped over, and menu
// deadlines elapsed unseen -- random failures that never reproduced on a
// fast many-core workstation.
//
// Thread participation:
//   - A thread joins automatically on its first sim blocking call
//     (advance_to/wait_until/park/mutex_wait_*), or explicitly with
//     thread_attach() (FreeRTOS task entry). It leaves at thread exit
//     (TLS guard) or thread_detach().
//   - Blocking calls no longer drive the clock; they register a deadline
//     and sleep. vTaskDelay/arch_delay_ms/script waits are advance_to().
//   - Binary-semaphore and task-notify takes are wait_until(): woken by
//     kick() (a give) at frozen time, or by their deadline. On deadline
//     expiry the waiter gets one quiescence grace: it reports the timeout
//     only after every other thread due at the same instant has run and
//     blocked again, so an event landing exactly at the deadline (the RX
//     task draining the awaited frame) is never missed.
//   - Firmware mutexes are wall-blocking OS mutexes, but a thread waiting
//     on one marks itself with mutex_wait_begin()/end(), so it counts as
//     blocked: a holder that itself waits on sim time across the lock
//     (sv_mutex across a save flush, the critical section across the
//     MODE-restart delay) still lets the clock reach its deadline.
//   - Firmware spin loops (the menu input loop polls pads and the MODE
//     detector with no delay) would keep a thread runnable forever and
//     freeze the clock. spin_poll(), called from the mtimer shim, counts
//     clock reads since the last sim block and turns a long spin into a
//     block until the next chunk, bounding iterations per sim-step.
//   - park() is for threads that must never run again but cannot exit
//     (vTaskStartScheduler, the system-reset stub awaiting re-exec).
//   - set_thread_inhibits(false) makes a thread an observer: its own
//     waits still work, but being runnable does not hold the clock (the
//     interactive terminal thread, blocked in read() on stdin).
//
// With the fake-FPGA backend, stepping is free (the fake answers
// synchronously in the caller's thread). With an RTL backend, stepping
// clocks the Verilator model (see Backend::run_until): long idle
// stretches are skipped without clocking by the bridge, so only real bus
// activity costs wall time.
//
// Deadlock rule: the clock can only stall if every participant is blocked
// with no finite deadline and no kick ever comes (the scheduler logs this
// if it persists). In practice the firmware always has a finite deadline
// outstanding (RX/watchdog delays) and the script/supervisor threads wait
// with finite deadlines, so time always has a next step while anyone
// waits on it.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>

namespace sim {

// One tick = one 21.492 MHz model clock. Firmware milliseconds are exact
// multiples of this (21492 ticks/ms); microseconds round to a tick.
constexpr uint64_t TICKS_PER_MS = 21492;
// Largest single step the scheduler takes (5 ms). Keeps UART frame traffic,
// which the firmware resyncs after a 50 ms gap, flowing across advances.
constexpr uint64_t STEP_CHUNK = 5 * TICKS_PER_MS;
constexpr uint64_t INF = UINT64_MAX;

uint64_t now_ticks();
inline uint64_t now_ms() {
    return now_ticks() / TICKS_PER_MS;
}
inline uint64_t now_us() {
    return now_ticks() * 1000u / TICKS_PER_MS;
}

// A model backend. run_until() brings the model from its current tick to
// `target` (bit-level UART, timers, SDRAM...). Called only by the
// scheduler thread, with no sim locks held; it must not call advance_to /
// wait_until (kick() from an interrupt hook run inside it is fine: the
// wake is deferred until the step completes).
class Backend {
  public:
    virtual ~Backend() = default;
    virtual void run_until(uint64_t target_ticks) = 0;
};

void set_backend(Backend *b);

// Block until the clock reaches `target` (return at once if already
// there). The scheduler steps the model while this thread sleeps.
void advance_to(uint64_t target);
inline void advance_by(uint64_t dt) {
    advance_to(now_ticks() + dt);
}

// Wake every event waiter to re-check its predicate at the current time
// (an event landed that no clock step produced, e.g. a semaphore give or
// task notify). Callable from any thread, including interrupt hooks.
void kick();

// Block until pred() is true or the clock reaches `deadline` (use INF to
// wait for the event only). Returns pred(); on expiry the event gets one
// quiescence grace (all threads due at the deadline run first) before
// false is reported. pred is called with no sim locks held.
bool wait_until(uint64_t deadline, const std::function<bool()> &pred);

// Block forever as a participant that never needs time to advance for
// itself (scheduler-parked threads awaiting process re-exec).
[[noreturn]] void park();

// Spin accounting for polling loops: called by the mtimer shim on every
// firmware clock read. After many reads without an intervening sim block,
// blocks until the next scheduler chunk so time can advance.
void spin_poll();

// Participation management (usually implicit; see the header comment).
void thread_attach();
void thread_detach();
void set_thread_inhibits(bool inhibits);

// Mark this thread blocked on a (wall-clock) mutex between begin/end, so
// the scheduler counts it as blocked while it waits for the holder.
void mutex_wait_begin();
void mutex_wait_end();

} // namespace sim
