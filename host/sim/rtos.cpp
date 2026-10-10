// Host sim FreeRTOS emulation: tasks are detached std::threads, ticks are
// milliseconds of the virtual simulation clock (see sim_time.hpp), and
// mutexes/semaphores/notifies are STL primitives with FreeRTOS-shaped APIs.
//
// Blocking with a timeout drives the sim clock forward (sim::wait_until);
// an infinite wait (portMAX_DELAY) only blocks for the event. Mutexes are
// pure wall-clock: the firmware only ever takes them with portMAX_DELAY and
// never holds one across a sim wait... except sv_mutex, which IS held across
// save flushes that advance the clock. That is safe: the holder itself drives
// the clock, so it always makes progress, and waiters wall-block until it
// lets go.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <pthread.h>
#include <thread>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "sim_time.hpp"

namespace {

struct TaskControlImpl {
    std::thread thr;
    std::mutex m;
    std::condition_variable cv;
    // Atomic so sim waits can poll it without holding m (holding the
    // scheduler lock while taking m deadlocks against givers, which take
    // m and then kick the scheduler).
    std::atomic<uint32_t> notify_count{0};
    std::atomic<bool> alive{true};
};

thread_local TaskControlImpl *tls_task = nullptr;

uint64_t ms_to_ticks(TickType_t ms) {
    if (ms == portMAX_DELAY)
        return sim::INF;
    // Clamp absurd timeouts (portMAX_DELAY passed where a delay was meant).
    uint64_t h = 3600ull * 1000 * sim::TICKS_PER_MS;
    uint64_t t = (uint64_t)ms * sim::TICKS_PER_MS;
    return t > h ? h : t;
}

} // namespace

struct TaskControl {
    TaskControlImpl impl;
};

struct SemaphoreControl {
    bool is_mutex;
    // Mutex flavor: recursive (firmware takes state_mutex reentrantly across
    // helpers). Pure wall-clock; see the note above.
    std::recursive_mutex mutex;
    // Binary-semaphore flavor: sim-time waits. `available` is atomic so the
    // wait can poll it without holding sem_m (see notify_count above).
    std::mutex sem_m;
    std::atomic<bool> available{false};
};

extern "C" {

BaseType_t xTaskCreate(TaskFunction_t pxTaskCode, const char *const pcName,
                       uint16_t usStackDepth, void *pvParameters,
                       UBaseType_t uxPriority, TaskHandle_t *pxCreatedTask) {
    (void)pcName;
    (void)usStackDepth;
    (void)uxPriority;
    TaskControl *tc = new TaskControl();
    tc->impl.thr = std::thread([tc, pxTaskCode, pvParameters]() {
        tls_task = &tc->impl;
        pxTaskCode(pvParameters);
        tc->impl.alive.store(false);
    });
    tc->impl.thr.detach();
    if (pxCreatedTask)
        *pxCreatedTask = tc;
    return pdPASS;
}

void vTaskStartScheduler(void) {
    // Tasks already run as threads; park the firmware's main thread forever.
    for (;;)
        std::this_thread::sleep_for(std::chrono::hours(24));
}

void vTaskDelay(TickType_t xTicksToDelay) {
    if (xTicksToDelay == 0) {
        std::this_thread::yield();
        return;
    }
    sim::advance_by(ms_to_ticks(xTicksToDelay));
}

void vTaskDelete(TaskHandle_t xTaskToDelete) {
    TaskControlImpl *self = tls_task;
    TaskControlImpl *other = xTaskToDelete ? &xTaskToDelete->impl : nullptr;
    if (!other || other == self) {
        if (self)
            self->alive.store(false);
        // End this thread. Only used by stubbed-out USB code; still, exit
        // the calling thread rather than returning into nowhere.
        pthread_exit(nullptr);
    }
    // Deleting other tasks is not supported; mark them dead so status checks
    // observe it.
    if (other)
        other->alive.store(false);
}

TickType_t xTaskGetTickCount(void) {
    return (TickType_t)sim::now_ms();
}

eTaskState eTaskGetState(TaskHandle_t xTask) {
    if (!xTask)
        return eInvalid;
    return xTask->impl.alive.load() ? eRunning : eDeleted;
}

namespace {
std::recursive_mutex g_critical;
}

void taskENTER_CRITICAL(void) {
    g_critical.lock();
}

void taskEXIT_CRITICAL(void) {
    g_critical.unlock();
}

void taskYIELD(void) {
    std::this_thread::yield();
}

uint32_t ulTaskNotifyTake(BaseType_t xClearCountOnExit, TickType_t xTicksToWait) {
    TaskControlImpl *self = tls_task;
    static TaskControlImpl fallback; // notify from a non-task thread: never blocks
    TaskControlImpl &t = self ? *self : fallback;
    uint64_t deadline = (xTicksToWait == portMAX_DELAY || xTicksToWait == 0)
                            ? (xTicksToWait == 0 ? sim::now_ticks() : sim::INF)
                            : sim::now_ticks() + ms_to_ticks(xTicksToWait);
    // The count is only decremented here (givers only increment), so a true
    // observation stays true until we clear it below.
    if (!sim::wait_until(deadline,
                         [&] { return t.notify_count.load(std::memory_order_acquire) > 0; }))
        return 0;
    std::lock_guard<std::mutex> lk(t.m);
    uint32_t ret = t.notify_count.load(std::memory_order_relaxed);
    if (ret > 0) {
        if (xClearCountOnExit)
            t.notify_count.store(0, std::memory_order_relaxed);
        else
            t.notify_count.store(ret - 1, std::memory_order_relaxed);
    }
    return ret;
}

BaseType_t xTaskNotifyGive(TaskHandle_t xTaskToNotify) {
    TaskControlImpl *t = xTaskToNotify ? &xTaskToNotify->impl : tls_task;
    if (!t)
        return pdFAIL;
    {
        std::lock_guard<std::mutex> lk(t->m);
        uint32_t n = t->notify_count.load(std::memory_order_relaxed);
        if (n < 0xffffff)
            t->notify_count.store(n + 1, std::memory_order_relaxed);
    }
    t->cv.notify_one();
    // The waiter may sit in a pure (INF) sim wait on the scheduler CV.
    sim::kick();
    return pdPASS;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    SemaphoreControl *s = new SemaphoreControl();
    s->is_mutex = true;
    return s;
}

SemaphoreHandle_t xSemaphoreCreateBinary(void) {
    SemaphoreControl *s = new SemaphoreControl();
    s->is_mutex = false;
    s->available.store(false, std::memory_order_relaxed);
    return s;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t xSemaphore, TickType_t xTicksToWait) {
    if (!xSemaphore)
        return pdFAIL;
    if (xSemaphore->is_mutex) {
        // Wall-clock: holders never wait on sim time while holding it... but
        // sv_mutex IS held across clock-advancing flushes. That still works:
        // the holder drives the clock itself, so it always finishes.
        std::recursive_mutex &m = xSemaphore->mutex;
        if (xTicksToWait == 0)
            return m.try_lock() ? pdTRUE : pdFALSE;
        if (xTicksToWait == portMAX_DELAY) {
            m.lock();
            return pdTRUE;
        }
        using namespace std::chrono;
        auto until = steady_clock::now() + milliseconds(xTicksToWait);
        while (!m.try_lock()) {
            if (steady_clock::now() >= until)
                return pdFALSE;
            std::this_thread::yield();
        }
        return pdTRUE;
    }
    uint64_t deadline = xTicksToWait == 0 ? sim::now_ticks()
                        : xTicksToWait == portMAX_DELAY
                            ? sim::INF
                            : sim::now_ticks() + ms_to_ticks(xTicksToWait);
    for (;;) {
        if (!sim::wait_until(deadline, [&] {
                return xSemaphore->available.load(std::memory_order_acquire);
            }))
            return pdFALSE;
        std::lock_guard<std::mutex> lk(xSemaphore->sem_m);
        if (xSemaphore->available.load(std::memory_order_relaxed)) {
            xSemaphore->available.store(false, std::memory_order_relaxed);
            return pdTRUE;
        }
        // Stolen by another taker between the wait and the lock (no firmware
        // path does this today): wait out the remaining time.
        if (deadline != sim::INF && sim::now_ticks() >= deadline)
            return pdFALSE;
    }
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t xSemaphore) {
    if (!xSemaphore)
        return pdFAIL;
    if (xSemaphore->is_mutex) {
        try {
            xSemaphore->mutex.unlock();
        } catch (...) {
            return pdFAIL;
        }
        return pdPASS;
    }
    {
        std::lock_guard<std::mutex> lk(xSemaphore->sem_m);
        xSemaphore->available.store(true, std::memory_order_relaxed);
    }
    // Wake pure (INF) waiters: no clock advance produced this event.
    sim::kick();
    return pdPASS;
}

BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t xSemaphore, void *woken) {
    (void)woken;
    return xSemaphoreGive(xSemaphore);
}

} // extern "C"

namespace sim_rtos {
void init_timebase() {
    // Fresh process: the sim clock starts at zero.
}
uint64_t uptime_ms() {
    return sim::now_ms();
}
} // namespace sim_rtos

