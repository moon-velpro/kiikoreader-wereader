#include "weread_prefetch.h"
#include <string.h>
#include <stdlib.h>
#include "esp_heap_caps.h"

static void wr_prefetch_thread(void *arg) {
    wr_prefetch_t *state = arg;
    for (;;) {
        xSemaphoreTake(state->kick, portMAX_DELAY);
        if (state->stop) {
            xSemaphoreGive(state->done);
            vTaskSuspend(NULL);
        }
        size_t n = 0;
        bool plain = false;
        void *data = state->fetch(state->context, state->target, &n, &plain);
        state->result = data;
        state->size = n;
        state->plain = plain;
        xSemaphoreGive(state->done);
    }
}

bool wr_prefetch_begin(wr_prefetch_t *state, wr_prefetch_fetch_fn fn,
                       wr_prefetch_release_fn release, void *context) {
    if (!state) return false;
    memset(state, 0, sizeof(*state));
    if (!fn) return false;
#if defined(CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM) && CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) < 6 * 1024 * 1024) return false;
    state->fetch = fn;
    state->release = release;
    state->context = context;
    state->kick = xSemaphoreCreateBinary();
    state->done = xSemaphoreCreateBinary();
    if (state->kick && state->done &&
        xTaskCreateWithCaps(wr_prefetch_thread, "wr_prefetch", 12 * 1024, state, 1,
                            &state->task, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS) return true;
    if (state->kick) vSemaphoreDelete(state->kick);
    if (state->done) vSemaphoreDelete(state->done);
    memset(state, 0, sizeof(*state));
#else
    (void)fn;
    (void)context;
#endif
    return false;
}

bool wr_prefetch_arm(wr_prefetch_t *state, unsigned chapter) {
    if (!state || !state->task || state->active || state->stop) return false;
    state->target = chapter;
    state->result = NULL;
    state->size = 0;
    state->plain = false;
    state->active = true;
    xSemaphoreGive(state->kick);
    return true;
}

void *wr_prefetch_take(wr_prefetch_t *state, unsigned chapter, size_t *size, bool *plain) {
    if (!state || !state->active || state->target != chapter) return NULL;
    xSemaphoreTake(state->done, portMAX_DELAY);
    void *result = state->result;
    if (size) *size = state->size;
    if (plain) *plain = state->plain;
    state->result = NULL;
    state->active = false;
    return result;
}

void wr_prefetch_end(wr_prefetch_t *state) {
    if (!state || !state->task) return;
    if (state->active) {
        xSemaphoreTake(state->done, portMAX_DELAY);
        if (state->release) state->release(state->result);
        else free(state->result);
        state->result = NULL;
        state->active = false;
    }
    state->stop = true;
    xSemaphoreGive(state->kick);
    xSemaphoreTake(state->done, portMAX_DELAY);
    vTaskDeleteWithCaps(state->task);
    vSemaphoreDelete(state->kick);
    vSemaphoreDelete(state->done);
    memset(state, 0, sizeof(*state));
}
