// Host sim shim for FreeRTOS semphr.h.
#pragma once

#include "FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

struct SemaphoreControl;
typedef struct SemaphoreControl *SemaphoreHandle_t;

SemaphoreHandle_t xSemaphoreCreateMutex(void);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t xSemaphore, TickType_t xTicksToWait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t xSemaphore);
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t xSemaphore, void *pxHigherPriorityTaskWoken);

#ifdef __cplusplus
}
#endif
