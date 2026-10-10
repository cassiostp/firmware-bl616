// Virtual simulation clock for tangcore-sim.
//
// Wall-clock time made the scripted tests flaky (a scripted button press
// could be held long enough, in wall time, to auto-repeat). All firmware
// time now derives from this clock, which only moves when some thread drives
// it forward:
//
//   - vTaskDelay/arch_delay_ms advance it directly (nothing else to wait for).
//   - Binary-semaphore takes and task-notify takes with a timeout drive it
//     while waiting for their event.
//   - The script runner advances it while polling expectations.
//   - The supervisor advances it while waiting for a firmware reset.
//
// Driving is done in small chunks (STEP_CHUNK, 5 ms) with a wall-clock yield
// between chunks, so the firmware's UART RX task keeps draining frames: its
// 50 ms inter-byte resync rule measures this same clock, and a single atomic
// jump past it would drop frames.
//
// With the fake-FPGA backend, advancing is free (the fake answers
// synchronously). With an RTL backend, advancing clocks the Verilator model
// (see Backend::run_until): long idle stretches are skipped without clocking
// by the bridge, so only real bus activity costs wall time.
//
// Deadlock rule: a thread holding no other lock either drives the clock
// (finite deadline) or sleeps purely on the condition variable (infinite
// deadline, woken by someone else's advance). The firmware always has a
// driver (its delay loops), and the script thread always drives, so time
// cannot stall while anyone waits on it.
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
// `target` (bit-level UART, timers, SDRAM...). Called with the scheduler
// lock held; must not call back into sim:: functions.
class Backend {
  public:
    virtual ~Backend() = default;
    virtual void run_until(uint64_t target_ticks) = 0;
};

void set_backend(Backend *b);

// Move the clock to `target` (no-op if already there), stepping the model.
// Broadcasts to waiters at every chunk and yields wall-clock time between
// chunks so other threads run.
void advance_to(uint64_t target);
inline void advance_by(uint64_t dt) {
    advance_to(now_ticks() + dt);
}

// Wake every waiter without moving the clock (an event landed that no
// advance produced, e.g. a semaphore give or task notify).
void kick();

// Block until pred() is true or the clock reaches `deadline` (use INF to
// wait for the event only, without driving). A finite deadline drives the
// clock in chunks while waiting. Returns pred(); on expiry the event gets a
// short wall-clock grace (threads woken by our own advance may still be
// landing it) before false is reported.
bool wait_until(uint64_t deadline, const std::function<bool()> &pred);

} // namespace sim
