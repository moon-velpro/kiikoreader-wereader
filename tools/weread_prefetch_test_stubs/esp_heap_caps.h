#pragma once
#include <stddef.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
size_t heap_caps_get_free_size(unsigned);
