/* tiny_port_host.c - Linux/macOS port used by the host tests. */
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include "tiny_port.h"

void *tiny_port_alloc(size_t size, tiny_mem_t kind)
{
    (void)kind;
    size = (size + 15) & ~(size_t)15;
    return aligned_alloc(16, size ? size : 16);
}

void tiny_port_free(void *p) { free(p); }
size_t tiny_port_fast_largest(void) { return SIZE_MAX; }
size_t tiny_port_fast_free(void) { return SIZE_MAX; }

uint32_t tiny_port_random(void)
{
    static uint32_t s;
    if (!s) s = (uint32_t)time(NULL) * 2654435761u | 1u;
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s;
}

uint64_t tiny_port_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

void tiny_port_yield(void) {}
