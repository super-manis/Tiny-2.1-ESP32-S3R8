/* tiny_port_esp.c - ESP-IDF port: SRAM/PSRAM placement, timing, yielding. */
#include <stdint.h>
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tiny_port.h"

#define CAPS_FAST (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define CAPS_BULK (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

void *tiny_port_alloc(size_t size, tiny_mem_t kind)
{
    return heap_caps_aligned_alloc(16, size, kind == TINY_MEM_FAST ? CAPS_FAST : CAPS_BULK);
}

void tiny_port_free(void *p)
{
    heap_caps_free(p);
}

size_t tiny_port_fast_largest(void)
{
    return heap_caps_get_largest_free_block(CAPS_FAST);
}

size_t tiny_port_fast_free(void)
{
    return heap_caps_get_free_size(CAPS_FAST);
}

uint32_t tiny_port_random(void)
{
    return esp_random();
}

uint64_t tiny_port_time_us(void)
{
    return (uint64_t)esp_timer_get_time();
}

/* Higher priority tasks (LVGL, input, audio) preempt us anyway. This only
 * makes sure tasks at or below our priority - including the IDLE task that
 * feeds the task watchdog - get a tick now and then during long answers. */
void tiny_port_yield(void)
{
    static int64_t last;
    int64_t now = esp_timer_get_time();
    if (now - last > 250000) {
        last = now;
        vTaskDelay(1);
    }
}
