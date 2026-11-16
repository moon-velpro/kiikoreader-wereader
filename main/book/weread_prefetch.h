#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

typedef void *(*wr_prefetch_fetch_fn)(void *context, unsigned chapter, size_t *size, bool *plain);
typedef void (*wr_prefetch_release_fn)(void *result);
typedef struct {
    TaskHandle_t task;
    SemaphoreHandle_t kick, done;
    wr_prefetch_fetch_fn fetch;
    wr_prefetch_release_fn release;
    void *context;
    unsigned target;
    size_t size;
    void *result;
    bool plain;
    bool active;
    volatile bool stop;
} wr_prefetch_t;

bool wr_prefetch_begin(wr_prefetch_t *state, wr_prefetch_fetch_fn fn,
                       wr_prefetch_release_fn release, void *context);
bool wr_prefetch_arm(wr_prefetch_t *state, unsigned chapter);
void *wr_prefetch_take(wr_prefetch_t *state, unsigned chapter, size_t *size, bool *plain);
void wr_prefetch_end(wr_prefetch_t *state);
