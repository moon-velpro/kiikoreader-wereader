#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "../main/apps/weread_progress_policy.h"

int main(void) {
    assert(weread_progress_should_redraw(0, 0, 0, 32, true));
    assert(!weread_progress_should_redraw(0, 32, 1, 32, true));
    assert(weread_progress_should_redraw(0, 32, 2, 32, true));
    assert(!weread_progress_should_redraw(2, 32, 3, 32, true));
    assert(weread_progress_should_redraw(2, 32, 4, 32, true));
    assert(!weread_progress_should_redraw(30, 32, 32, 32, true));
    assert(!weread_progress_should_redraw(2, 32, 2, 32, true));
    assert(weread_progress_should_redraw(0, 32, 1, 32, false));
    assert(!weread_progress_should_redraw(1, 32, 1, 32, false));
    assert(!weread_progress_should_redraw(0, 0, 1, 0, true));
    assert(weread_progress_should_redraw(0, 0, 0, 4, false));
    puts("PASS: cache 5% coalescing, terminal-frame suppression, other jobs unchanged");
    return 0;
}
