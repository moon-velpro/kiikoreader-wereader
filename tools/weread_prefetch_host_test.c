#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include "../main/book/weread_prefetch.h"

size_t heap_caps_get_free_size(unsigned caps) { (void)caps; return 64*1024*1024; }
struct ctx { TaskFunction_t fn; void *arg; };
static void *entry(void *p) {
    struct ctx *ctx = p;
    TaskFunction_t fn = ctx->fn;
    void *arg = ctx->arg;
    free(ctx);
    fn(arg);
    return NULL;
}
BaseType_t xTaskCreateWithCaps(TaskFunction_t fn, const char *name, unsigned size,
    void *arg, unsigned priority, TaskHandle_t *out, unsigned caps) {
    (void)name; (void)size; (void)priority; (void)caps;
    *out = malloc(sizeof(**out));
    struct ctx *x = malloc(sizeof(*x));
    assert(*out && x);
    x->fn = fn; x->arg = arg;
    assert(pthread_create(&(*out)->thread, NULL, entry, x) == 0);
    return pdPASS;
}
void vTaskDeleteWithCaps(TaskHandle_t t) {
    pthread_cancel(t->thread);
    pthread_join(t->thread, NULL);
    free(t);
}
void vTaskSuspend(void *ignored) { (void)ignored; for (;;) pause(); }
SemaphoreHandle_t xSemaphoreCreateBinary(void) {
    sem_t *s = malloc(sizeof(*s));
    assert(s && sem_init(s, 0, 0) == 0);
    return s;
}
int xSemaphoreGive(SemaphoreHandle_t s) { return sem_post(s) == 0; }
int xSemaphoreTake(SemaphoreHandle_t s, unsigned ms) {
    (void)ms;
    int n;
    do { n = sem_wait(s); } while (n && errno == EINTR);
    assert(n == 0);
    return 1;
}
void vSemaphoreDelete(SemaphoreHandle_t s) { sem_destroy(s); free(s); }
static void *fetch(void *ctx, unsigned ch, size_t *size, bool *plain) {
    (void)ctx;
    char *p = malloc(64);
    assert(p);
    sprintf(p, "chapter-%u", ch);
    *size = strlen(p); *plain = true;
    usleep(10000);
    return p;
}
int main(void) {
    wr_prefetch_t state;
    assert(wr_prefetch_begin(&state, fetch, free, NULL));
    for (unsigned i = 0; i < 32; ++i) {
        assert(wr_prefetch_arm(&state, i));
        assert(!wr_prefetch_arm(&state, i+1));
        size_t n = 0; bool plain = false;
        char *value = wr_prefetch_take(&state, i, &n, &plain);
        char expected[64];
        sprintf(expected, "chapter-%u", i);
        assert(value && plain && n == strlen(expected) && !strcmp(value, expected));
        free(value);
    }
    assert(wr_prefetch_arm(&state, 500));
    wr_prefetch_end(&state);
    puts("PASS: 32-chapter prefetch, bounded queue, result ownership, active shutdown");
    return 0;
}
