// Unit test for the deterministic sim scheduler (host/sim/sim_time.cpp).
//
// Exercises the properties the scripted suite relies on, in wall-time
// terms a regression cannot fake:
//  1. The clock does not advance while a participating thread is runnable
//     (a timed waiter is not expired by other threads' activity).
//  2. An event landing exactly at a waiter's deadline -- set by another
//     thread woken at the same instant -- is observed, not timed out
//     (the quiescence grace; the RX-task/core-ID-poll race in miniature).
//  3. A polling loop that only reads the clock still makes time progress
//     (spin_poll turns the spin into chunked sleeps; the menu loop).
//  4. A plain timeout expires at its deadline, neither early nor by a
//     chunk more than the step granularity.
//
// The test's main thread is a non-inhibiting observer so it can join
// workers without holding the clock itself. A wall watchdog fails the
// test instead of hanging if the scheduler ever deadlocks.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "sim_time.hpp"

namespace {

int g_failures = 0;

void check(bool ok, const char *name) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok)
        g_failures++;
}

uint64_t ms(uint64_t n) {
    return n * sim::TICKS_PER_MS;
}

void test_frozen_while_runnable() {
    std::atomic<bool> waiter_done{false};
    std::atomic<bool> spinner_ready{false};
    std::atomic<bool> release{false};
    uint64_t t0 = sim::now_ticks();

    // The spinner attaches first, so a registered runnable participant
    // exists before the waiter can block: otherwise the waiter alone
    // could legitimately be stepped to its deadline before the spinner
    // thread has ever been scheduled by the host.
    std::thread spinner([&] {
        sim::thread_attach();
        spinner_ready.store(true);
        // Runnable (wall-spinning) the whole time: the clock must stay
        // at t0 even though the waiter's deadline is pending.
        while (!release.load())
            std::this_thread::yield();
        sim::advance_to(t0 + ms(50));
    });
    while (!spinner_ready.load())
        std::this_thread::yield();
    std::thread waiter([&] {
        sim::advance_to(t0 + ms(50));
        waiter_done.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    check(sim::now_ticks() == t0 && !waiter_done.load(),
          "clock frozen while a participant is runnable");
    release.store(true);
    waiter.join();
    spinner.join();
    check(waiter_done.load() && sim::now_ticks() >= t0 + ms(50),
          "timed wait completes once everyone blocks");
}

void test_event_at_deadline() {
    std::atomic<bool> event{false};
    std::atomic<bool> observed{false};
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    uint64_t t0 = sim::now_ticks();

    // Both threads attach (registered + runnable, so the clock is frozen)
    // and only enter their waits once both are attached: otherwise the
    // waiter could legitimately expire before the setter thread has even
    // been scheduled by the host, which is not the race under test.
    auto attach_and_wait_go = [&] {
        sim::thread_attach();
        ready.fetch_add(1);
        while (!go.load())
            std::this_thread::yield();
    };
    std::thread waiter([&] {
        attach_and_wait_go();
        bool r = sim::wait_until(t0 + ms(100), [&] { return event.load(); });
        observed.store(r);
    });
    std::thread setter([&] {
        attach_and_wait_go();
        // Wakes at the same instant as the waiter's deadline and only
        // then lands the event: without the grace, whichever thread the
        // host schedules first decides the result.
        sim::advance_to(t0 + ms(100));
        event.store(true);
        sim::kick();
        sim::advance_to(t0 + ms(200)); // block again: let quiescence happen
    });

    while (ready.load() < 2)
        std::this_thread::yield();
    go.store(true);
    waiter.join();
    setter.join();
    check(observed.load(), "event landing at the deadline is observed");
}

void test_spin_progress() {
    std::atomic<bool> done{false};
    uint64_t t0 = sim::now_ticks();
    std::thread spinner([&] {
        sim::thread_attach();
        // No sim waits at all, like the firmware menu loop: spin_poll
        // must turn this into chunked sleeps so the deadline arrives.
        while (sim::now_ticks() - t0 < ms(50))
            sim::spin_poll();
        done.store(true);
    });
    spinner.join();
    check(done.load() && sim::now_ticks() >= t0 + ms(50),
          "clock-read spin loop makes time progress");
}

void test_timeout_expires() {
    std::atomic<bool> result{true};
    std::atomic<uint64_t> at{0};
    uint64_t t0 = sim::now_ticks();
    std::thread waiter([&] {
        bool r = sim::wait_until(t0 + ms(30), [] { return false; });
        result.store(r);
        at.store(sim::now_ticks());
    });
    waiter.join();
    check(!result.load() && at.load() >= t0 + ms(30) &&
              at.load() <= t0 + ms(30) + sim::STEP_CHUNK,
          "timeout expires at its deadline");
}

} // namespace

int main() {
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        std::fprintf(stderr, "sim-time-test: wall watchdog fired (deadlock?)\n");
        std::_Exit(2);
    }).detach();

    sim::set_thread_inhibits(false); // observer: joining must not hold time

    test_frozen_while_runnable();
    test_event_at_deadline();
    test_spin_progress();
    test_timeout_expires();

    if (g_failures) {
        std::printf("sim-time-test: %d failures\n", g_failures);
        return 1;
    }
    std::printf("sim-time-test: all passed (sim %.1f ms)\n",
                sim::now_ms() * 1.0);
    return 0;
}
