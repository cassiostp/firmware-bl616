// Host sim FreeRTOS emulation: tasks are detached std::threads, ticks are
// milliseconds of steady-clock time, mutexes/semaphores/notifies are STL
// primitives with FreeRTOS-shaped APIs.
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

namespace {

std::chrono::steady_clock::time_point g_start;

struct TaskControlImpl {
    std::thread thr;
    std::mutex m;
    std::condition_variable cv;
    uint32_t notify_count = 0;
    std::atomic<bool> alive{true};
};

thread_local TaskControlImpl *tls_task = nullptr;

uint64_t now_ms() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - g_start)
        .count();
}

} // namespace

struct TaskControl {
    TaskControlImpl impl;
};

struct SemaphoreControl {
    bool is_mutex;
    // Mutex flavor: recursive (firmware takes state_mutex from one thread at a
    // time, but recursive is safer across helpers).
    std::recursive_timed_mutex mutex;
    // Binary-semaphore flavor.
    std::mutex sem_m;
    std::condition_variable sem_cv;
    bool available = false;
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
    if (xTicksToDelay)
        std::this_thread::sleep_for(std::chrono::milliseconds(xTicksToDelay));
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
    return (TickType_t)now_ms();
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
    std::unique_lock<std::mutex> lk(t.m);
    if (t.notify_count == 0 && xTicksToWait != 0) {
        if (xTicksToWait == portMAX_DELAY) {
            t.cv.wait(lk, [&] { return t.notify_count > 0; });
        } else {
            t.cv.wait_for(lk, std::chrono::milliseconds(xTicksToWait),
                          [&] { return t.notify_count > 0; });
        }
    }
    uint32_t ret = t.notify_count;
    if (ret > 0) {
        if (xClearCountOnExit)
            t.notify_count = 0;
        else
            t.notify_count--;
    }
    return ret;
}

BaseType_t xTaskNotifyGive(TaskHandle_t xTaskToNotify) {
    TaskControlImpl *t = xTaskToNotify ? &xTaskToNotify->impl : tls_task;
    if (!t)
        return pdFAIL;
    {
        std::lock_guard<std::mutex> lk(t->m);
        if (t->notify_count < 0xffffff)
            t->notify_count++;
    }
    t->cv.notify_one();
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
    s->available = false;
    return s;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t xSemaphore, TickType_t xTicksToWait) {
    if (!xSemaphore)
        return pdFAIL;
    if (xSemaphore->is_mutex) {
        std::recursive_timed_mutex &m = xSemaphore->mutex;
        bool ok;
        if (xTicksToWait == 0)
            ok = m.try_lock();
        else if (xTicksToWait == portMAX_DELAY) {
            m.lock();
            ok = true;
        } else {
            ok = m.try_lock_for(std::chrono::milliseconds(xTicksToWait));
        }
        return ok ? pdTRUE : pdFALSE;
    }
    std::unique_lock<std::mutex> lk(xSemaphore->sem_m);
    if (!xSemaphore->available && xTicksToWait != 0) {
        if (xTicksToWait == portMAX_DELAY) {
            xSemaphore->sem_cv.wait(lk, [&] { return xSemaphore->available; });
        } else {
            xSemaphore->sem_cv.wait_for(lk, std::chrono::milliseconds(xTicksToWait),
                                        [&] { return xSemaphore->available; });
        }
    }
    if (!xSemaphore->available)
        return pdFALSE;
    xSemaphore->available = false;
    return pdTRUE;
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
        xSemaphore->available = true;
    }
    xSemaphore->sem_cv.notify_one();
    return pdPASS;
}

BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t xSemaphore, void *woken) {
    (void)woken;
    return xSemaphoreGive(xSemaphore);
}

} // extern "C"

namespace sim_rtos {
void init_timebase() {
    g_start = std::chrono::steady_clock::now();
}
uint64_t uptime_ms() {
    return now_ms();
}
} // namespace sim_rtos
