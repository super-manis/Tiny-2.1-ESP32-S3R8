/*
 * main.c - Standalone ESP-IDF entry point for TINY 2.1 on an ESP32-S3-R8.
 *
 * When the watch OS integrates TINY as a component, it calls tiny_watch_start()
 * and tiny_watch_ui_create() (or tiny_watch_ask_async()) directly and does not
 * compile this file. As a standalone firmware image this:
 *   1. Starts the TINY watch service (weights split across SRAM + Octal PSRAM,
 *      worker pinned to core 1).
 *   2. Runs a self-test + benchmark across the three numeric kernels.
 *   3. Drops into a serial console so questions can be typed over USB-JTAG / UART.
 */
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tiny_watch.h"
#include "tiny_watch_ui.h"

static const char *TAG = "tiny_main";

static const struct {
    const char *q;
    const char *expect;
} selftest[] = {
    { "what is the capital of peru",           "Lima." },
    { "is a whale a fish",                     "No, a whale is a mammal." },
    { "what is 7 times 8",                     "7 times 8 is 56." },
    { "spell elephant",                        "E-L-E-P-H-A-N-T." },
    { "which countries border estonia",        "Latvia and Russia." },
    { "how do you say thank you in spanish",   "In Spanish it is gracias." },
    { "tell me a joke",                        "Why did the PC go to the doctor? It had a virus." },
};

static void stream_char(void *u, char c, const tiny_token_info_t *info)
{
    (void)u; (void)info;
    putchar(c);
    fflush(stdout);
}

static void stream_done(void *u, tiny_err_t status, const char *answer, const tiny_stats_t *st)
{
    (void)status; (void)answer;
    if (st)
        printf("\n   [%d letters in %u ms (prefill %u ms, decode %u ms) -> %.1f letters/s, conf %.0f%%]\n",
               st->answer_letters,
               (unsigned)((st->prefill_us + st->decode_us) / 1000),
               (unsigned)(st->prefill_us / 1000),
               (unsigned)(st->decode_us / 1000),
               (double)st->letters_per_s,
               (double)(st->mean_confidence * 100.0f));
    xSemaphoreGive((SemaphoreHandle_t)u);
}

void app_main(void)
{
    ESP_LOGI(TAG, "TINY 2.1 for Waveshare ESP32-S3-R8");
    ESP_LOGI(TAG, "free heap before load: %u KB internal, %u KB PSRAM",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    tiny_watch_t *w = NULL;
    tiny_err_t err = tiny_watch_start(NULL, &w);
    if (err != TINY_OK) {
        ESP_LOGE(TAG, "tiny_watch_start failed: %s", tiny_err_str(err));
        return;
    }

    ESP_LOGI(TAG, "free heap after  load: %u KB internal, %u KB PSRAM",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    /* Self-test with greedy decoding (creativity 0). */
    int pass = 0, n = (int)(sizeof(selftest) / sizeof(selftest[0]));
    uint32_t total_us = 0, total_letters = 0;
    for (int i = 0; i < n; i++) {
        char ans[TINY_CTX + 1];
        tiny_stats_t st;
        tiny_watch_ask(w, selftest[i].q, 0, ans, sizeof ans, &st);
        int ok = strcmp(ans, selftest[i].expect) == 0;
        pass += ok;
        total_us += st.prefill_us + st.decode_us;
        total_letters += (uint32_t)(st.question_letters + 1 + st.answer_letters);
        printf("  Q: %s\n  A: %s  [%.1f chr/s, %s]\n",
               selftest[i].q, ans, (double)st.letters_per_s, ok ? "OK" : "DIFF");
    }
    ESP_LOGI(TAG, "self-test: %d/%d matched, overall %.1f letters/s (%u letters in %u ms)",
             pass, n,
             total_us ? (double)total_letters * 1e6 / (double)total_us : 0.0,
             (unsigned)total_letters, (unsigned)(total_us / 1000));

    /* Interactive serial console (works over USB-Serial-JTAG or UART0). */
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    char line[160];
    int pos = 0;
    printf("\nAsk TINY anything (0..9 sets creativity, quit exits):\n> ");
    fflush(stdout);
    for (;;) {
        int ch = getchar();
        if (ch == EOF) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            putchar('\n');
            line[pos] = 0;
            pos = 0;
            if (line[0] >= '0' && line[0] <= '9' && line[1] == 0) {
                tiny_watch_set_creativity(w, line[0] - '0');
                printf("   [creativity %c]\n> ", line[0]);
                fflush(stdout);
                continue;
            }
            if (strcmp(line, "quit") == 0) break;
            if (line[0] != 0) {
                printf("  ");
                tiny_watch_req_t r = {
                    .question = line,
                    .creativity = -1,
                    .kernel = TINY_KERNEL_PRECISE,
                    .on_token = stream_char,
                    .on_done = stream_done,
                    .user = done,
                };
                if (tiny_watch_ask_async(w, &r) == TINY_OK)
                    xSemaphoreTake(done, portMAX_DELAY);
            }
            printf("\n> ");
            fflush(stdout);
        } else if ((ch == 8 || ch == 127) && pos > 0) {
            pos--;
            printf("\b \b");
            fflush(stdout);
        } else if (ch >= 32 && ch < 127 && pos + 1 < (int)sizeof(line)) {
            line[pos++] = (char)ch;
            putchar(ch);
            fflush(stdout);
        }
    }
}
