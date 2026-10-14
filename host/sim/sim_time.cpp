// Virtual simulation clock: deterministic discrete-event scheduler.
// See sim_time.hpp for the design.
#include "sim_time.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <thread>

namespace sim {

namespace {

enum class TState { RUNNABLE, TIME, EVENT, EXPIRED, MUTEX, PARKED };

struct ThreadInfo {
    TState state = TState::RUNNABLE;
    uint64_t deadline = INF; // TIME / EVENT
    bool inhibits = true;    // runnable inhibits clock advance
    bool wake = false;       // scheduler/kick asked this thread to run
};

struct State {
    std::mutex m;
    std::condition_variable cv;
    std::map<std::thread::id, ThreadInfo> threads; // node-stable refs
    Backend *backend = nullptr;
    bool stepping = false;      // scheduler is inside run_until
    bool kick_during_step = false;
};

// Leaked on purpose: TLS guards unregister threads during thread/process
// teardown, after ordinary statics may already be destroyed.
State &S() {
    static State *s = new State();
    return *s;
}

// The clock itself. Atomic so observing time (now_ticks) never contends
// with the scheduler: the firmware reads the clock constantly (per UART
// byte in the RX task's resync check, per menu poll). Only the scheduler
// thread writes it, at the end of a step.
std::atomic<uint64_t> g_ticks{0};

struct NullBackend : Backend {
    void run_until(uint64_t) override {}
};
NullBackend g_null;

thread_local bool tls_registered = false;
thread_local bool tls_is_scheduler = false;
thread_local int tls_spin = 0;

void unregister_self() {
    State &s = S();
    std::lock_guard<std::mutex> lk(s.m);
    s.threads.erase(std::this_thread::get_id());
    tls_registered = false;
    s.cv.notify_all();
}

struct TlsGuard {
    ~TlsGuard() {
        if (tls_registered)
            unregister_self();
    }
};
thread_local TlsGuard tls_guard;

void scheduler_main();

void start_scheduler() {
    static std::once_flag once;
    std::call_once(once, [] { std::thread(scheduler_main).detach(); });
}

void ensure_registered() {
    if (tls_registered || tls_is_scheduler)
        return;
    (void)tls_guard; // instantiate the exit guard on this thread
    State &s = S();
    {
        std::lock_guard<std::mutex> lk(s.m);
        s.threads[std::this_thread::get_id()] = ThreadInfo{};
        tls_registered = true;
        s.cv.notify_all();
    }
    start_scheduler();
}

[[noreturn]] void scheduler_thread_misuse(const char *what) {
    std::fprintf(stderr, "sim_time: %s called on the scheduler thread\n", what);
    std::fflush(stderr);
    std::_Exit(3);
}

// --- scheduler predicates (g_m held) ---

bool quiescent_locked(State &s) {
    bool any = false;
    for (auto &kv : s.threads) {
        if (!kv.second.inhibits)
            continue;
        any = true;
        // A thread with a wake pending is runnable for scheduling
        // purposes even though its state still says blocked: it has not
        // run since being woken, so time must not move past this instant
        // (and the scheduler must sleep, not spin, until it has run and
        // blocked again -- every re-block path notifies the CV).
        if (kv.second.state == TState::RUNNABLE || kv.second.wake)
            return false;
    }
    return any;
}

bool has_expired_locked(State &s) {
    for (auto &kv : s.threads)
        if (kv.second.state == TState::EXPIRED)
            return true;
    return false;
}

uint64_t earliest_deadline_locked(State &s) {
    uint64_t e = INF;
    for (auto &kv : s.threads) {
        if (kv.second.state == TState::TIME || kv.second.state == TState::EVENT)
            e = std::min(e, kv.second.deadline);
    }
    return e;
}

void scheduler_main() {
    tls_is_scheduler = true;
    State &s = S();
    std::unique_lock<std::mutex> lk(s.m);
    auto stalled_since = std::chrono::steady_clock::now();
    bool stall_warned = false;
    for (;;) {
        bool ready = quiescent_locked(s) &&
                     (has_expired_locked(s) || earliest_deadline_locked(s) != INF);
        if (!ready) {
            // Wake periodically only to spot a genuine stall (everyone
            // blocked with no deadline and no kick coming): log it once
            // so a hang is diagnosable instead of silent.
            if (s.cv.wait_for(lk, std::chrono::seconds(5)) == std::cv_status::timeout &&
                !s.threads.empty() && quiescent_locked(s) && !has_expired_locked(s) &&
                earliest_deadline_locked(s) == INF) {
                if (!stall_warned &&
                    std::chrono::steady_clock::now() - stalled_since >
                        std::chrono::seconds(15)) {
                    std::fprintf(stderr,
                                 "sim_time: clock stalled at %llu ms "
                                 "(all %zu threads blocked with no deadline)\n",
                                 (unsigned long long)now_ms(), s.threads.size());
                    std::fflush(stderr);
                    stall_warned = true;
                }
            }
            continue;
        }
        stalled_since = std::chrono::steady_clock::now();
        stall_warned = false;
        if (has_expired_locked(s)) {
            // Timeout grace: everyone else due at this instant has run
            // and blocked again. Let expired waiters re-check and return
            // before time moves any further.
            for (auto &kv : s.threads)
                if (kv.second.state == TState::EXPIRED)
                    kv.second.wake = true;
            s.cv.notify_all();
            continue;
        }
        uint64_t now = g_ticks.load(std::memory_order_relaxed);
        uint64_t step = std::min(earliest_deadline_locked(s), now + STEP_CHUNK);
        if (step <= now) { // a deadline already due: wake without stepping
            for (auto &kv : s.threads) {
                ThreadInfo &t = kv.second;
                if ((t.state == TState::TIME || t.state == TState::EVENT) &&
                    t.deadline <= now)
                    t.wake = true;
            }
            s.cv.notify_all();
            continue;
        }
        Backend *b = s.backend ? s.backend : (Backend *)&g_null;
        s.stepping = true;
        s.kick_during_step = false;
        lk.unlock();
        b->run_until(step);
        lk.lock();
        s.stepping = false;
        g_ticks.store(step, std::memory_order_relaxed);
        for (auto &kv : s.threads) {
            ThreadInfo &t = kv.second;
            if (t.state == TState::TIME && t.deadline <= step)
                t.wake = true;
            else if (t.state == TState::EVENT &&
                     (t.deadline <= step || s.kick_during_step))
                t.wake = true;
        }
        s.cv.notify_all();
    }
}

// Enter a blocked state and sleep until woken. Returns with the thread
// marked RUNNABLE and no lock held.
void block_until_woken(TState state, uint64_t deadline) {
    State &s = S();
    std::unique_lock<std::mutex> lk(s.m);
    ThreadInfo &me = s.threads[std::this_thread::get_id()];
    me.state = state;
    me.deadline = deadline;
    me.wake = false;
    s.cv.notify_all(); // scheduler: quiescence / earliest deadline changed
    s.cv.wait(lk, [&] { return me.wake; });
    me.state = TState::RUNNABLE;
    me.wake = false;
}

} // namespace

void set_backend(Backend *b) {
    State &s = S();
    {
        std::lock_guard<std::mutex> lk(s.m);
        s.backend = b ? b : (Backend *)&g_null;
    }
    start_scheduler();
}

void kick() {
    State &s = S();
    std::lock_guard<std::mutex> lk(s.m);
    if (s.stepping) {
        // From an interrupt hook inside run_until: defer the wake until
        // the scheduler finishes the step and publishes the new time.
        s.kick_during_step = true;
        return;
    }
    for (auto &kv : s.threads)
        if (kv.second.state == TState::EVENT)
            kv.second.wake = true;
    s.cv.notify_all();
}

uint64_t now_ticks() {
    return g_ticks.load(std::memory_order_relaxed);
}

void thread_attach() {
    ensure_registered();
}

void thread_detach() {
    if (tls_registered)
        unregister_self();
}

void set_thread_inhibits(bool inhibits) {
    ensure_registered();
    State &s = S();
    std::lock_guard<std::mutex> lk(s.m);
    s.threads[std::this_thread::get_id()].inhibits = inhibits;
    s.cv.notify_all();
}

void mutex_wait_begin() {
    ensure_registered();
    if (tls_is_scheduler)
        scheduler_thread_misuse("mutex_wait_begin");
    State &s = S();
    std::lock_guard<std::mutex> lk(s.m);
    ThreadInfo &me = s.threads[std::this_thread::get_id()];
    me.state = TState::MUTEX;
    me.deadline = INF;
    s.cv.notify_all();
}

void mutex_wait_end() {
    State &s = S();
    std::lock_guard<std::mutex> lk(s.m);
    auto it = s.threads.find(std::this_thread::get_id());
    if (it != s.threads.end() && it->second.state == TState::MUTEX) {
        it->second.state = TState::RUNNABLE;
        s.cv.notify_all();
    }
}

void advance_to(uint64_t target) {
    if (tls_is_scheduler)
        scheduler_thread_misuse("advance_to");
    ensure_registered();
    tls_spin = 0;
    if (now_ticks() >= target)
        return;
    block_until_woken(TState::TIME, target);
    // The scheduler only wakes TIME waiters once the clock reaches their
    // deadline, so now >= target here.
}

bool wait_until(uint64_t deadline, const std::function<bool()> &pred) {
    if (tls_is_scheduler)
        scheduler_thread_misuse("wait_until");
    ensure_registered();
    tls_spin = 0;
    // pred is always called with no sim locks held (it takes leaf locks
    // itself); while it runs this thread is RUNNABLE, so the clock is
    // frozen and the observation is at a well-defined instant.
    if (pred())
        return true;
    if (deadline != INF && now_ticks() >= deadline)
        return pred(); // zero/already-expired timeout: never blocked
    for (;;) {
        block_until_woken(TState::EVENT, deadline);
        tls_spin = 0;
        if (pred())
            return true;
        if (deadline == INF || now_ticks() < deadline)
            continue;
        // Deadline reached, event still missing: threads woken at the
        // same instant may still be landing it (e.g. the RX task draining
        // the awaited frame). Yield to quiescence once before reporting
        // the timeout -- the deterministic form of the old wall grace.
        block_until_woken(TState::EXPIRED, deadline);
        return pred();
    }
}

[[noreturn]] void park() {
    if (tls_is_scheduler)
        scheduler_thread_misuse("park");
    ensure_registered();
    State &s = S();
    std::unique_lock<std::mutex> lk(s.m);
    ThreadInfo &me = s.threads[std::this_thread::get_id()];
    me.state = TState::PARKED;
    me.deadline = INF;
    me.wake = false;
    s.cv.notify_all();
    for (;;)
        s.cv.wait(lk);
}

void spin_poll() {
    // Firmware polling loops read the clock to decide when to act; with a
    // frozen clock they would spin forever. Bound the work per instant:
    // after SPIN_LIMIT reads since the last sim block, sleep until the
    // next chunk like any other timed wait.
    constexpr int SPIN_LIMIT = 128;
    if (tls_is_scheduler)
        return;
    if (++tls_spin < SPIN_LIMIT)
        return;
    tls_spin = 0;
    advance_to(now_ticks() + STEP_CHUNK);
}

} // namespace sim
