#pragma once
#include "FreeRTOS.h"
#include <pthread.h>
typedef struct task_mock { pthread_t thread; } *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
BaseType_t xTaskCreateWithCaps(TaskFunction_t, const char *, unsigned, void *, unsigned, TaskHandle_t *, unsigned);
void vTaskDeleteWithCaps(TaskHandle_t);
void vTaskSuspend(void *);
