/* tiny_port.h - the few things the engine needs from the platform. */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef enum {
    TINY_MEM_FAST,   /* internal SRAM on the ESP32-S3 */
    TINY_MEM_BULK,   /* PSRAM on the ESP32-S3         */
} tiny_mem_t;

/* 16-byte aligned allocation. Returns NULL on failure (no fallback). */
void    *tiny_port_alloc(size_t size, tiny_mem_t kind);
void     tiny_port_free(void *p);
/* Largest block / total free bytes of fast memory (SIZE_MAX if unknown). */
size_t   tiny_port_fast_largest(void);
size_t   tiny_port_fast_free(void);
uint32_t tiny_port_random(void);
uint64_t tiny_port_time_us(void);
/* Called once per letter: lets lower-priority tasks / the idle task run. */
void     tiny_port_yield(void);
