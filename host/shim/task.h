// Host sim shim for FreeRTOS task.h.
#pragma once

#include "FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    eRunning = 0,
    eReady,
    eBlocked,
    eSuspended,
    eDeleted,
    eInvalid
} eTaskState;

BaseType_t xTaskCreate(TaskFunction_t pxTaskCode, const char *const pcName,
                       uint16_t usStackDepth, void *pvParameters,
                       UBaseType_t uxPriority, TaskHandle_t *pxCreatedTask);
void vTaskStartScheduler(void);
void vTaskDelay(TickType_t xTicksToDelay);
void vTaskDelete(TaskHandle_t xTaskToDelete);
TickType_t xTaskGetTickCount(void);
eTaskState eTaskGetState(TaskHandle_t xTask);

void taskENTER_CRITICAL(void);
void taskEXIT_CRITICAL(void);
void taskYIELD(void);

uint32_t ulTaskNotifyTake(BaseType_t xClearCountOnExit, TickType_t xTicksToWait);
BaseType_t xTaskNotifyGive(TaskHandle_t xTaskToNotify);

#ifdef __cplusplus
}
#endif
