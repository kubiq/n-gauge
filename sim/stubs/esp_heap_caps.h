/* Stand-in for ESP-IDF's heap: PSRAM is plain memory on the PC. C, not C++:
   LVGL's memory pool (the gauge's lv_conf.h) includes it from C sources. */
#pragma once
#include <stddef.h>
#include <stdlib.h>

#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_INTERNAL 0
#define MALLOC_CAP_8BIT 0
static inline void *heap_caps_malloc(size_t n, unsigned caps) { (void)caps; return malloc(n); }
static inline void *heap_caps_calloc(size_t n, size_t m, unsigned caps) { (void)caps; return calloc(n, m); }
static inline void heap_caps_free(void *p) { free(p); }
static inline size_t heap_caps_get_free_size(unsigned caps) { (void)caps; return 4u * 1024u * 1024u; }
