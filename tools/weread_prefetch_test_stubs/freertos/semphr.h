#pragma once
#include <semaphore.h>
typedef sem_t *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateBinary(void);
int xSemaphoreGive(SemaphoreHandle_t);
int xSemaphoreTake(SemaphoreHandle_t, unsigned);
void vSemaphoreDelete(SemaphoreHandle_t);
