// Host sim shim for FreeRTOS.h. Tasks are pthreads (via std::thread),
// ticks are milliseconds of wall-clock time.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t TickType_t;
typedef int32_t BaseType_t;
typedef uint32_t UBaseType_t;
typedef void (*TaskFunction_t)(void *arg);

struct TaskControl;
typedef struct TaskControl *TaskHandle_t;

#define pdTRUE ((BaseType_t)1)
#define pdFALSE ((BaseType_t)0)
#define pdPASS ((BaseType_t)1)
#define pdFAIL ((BaseType_t)0)

#define portMAX_DELAY ((TickType_t)0xffffffffu)
#define portTICK_PERIOD_MS ((TickType_t)1)
#define pdMS_TO_TICKS(x) ((TickType_t)(x))

#define configMAX_PRIORITIES 8

#ifdef __cplusplus
}
#endif
