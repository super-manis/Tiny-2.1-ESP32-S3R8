/*
 * tiny_watch.h - Watch-level service for TINY 2.1 on the ESP32-S3.
 *
 * Wraps the inference engine in a FreeRTOS worker task (pinned to the APP
 * core by default, so LVGL / touch / BLE on core 0 never stutter), raises
 * the CPU clock via an esp_pm lock only while answering, loads the weights
 * from a flash partition / filesystem / embedded blob, and includes the
 * classic TINY robot voice converted to 16-bit PCM for the watch speaker.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "tiny_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tiny_watch tiny_watch_t;

/* Called from the worker task as each letter of the answer is produced. */
typedef void (*tiny_watch_token_fn)(void *user, char letter, const tiny_token_info_t *info);

/* Called from the worker task when the answer is complete (or cancelled). */
typedef void (*tiny_watch_done_fn)(void *user, tiny_err_t status, const char *answer,
                                   const tiny_stats_t *stats);

/* Optional: push 16-bit signed mono PCM samples to the watch's audio codec
 * (e.g. ES8311 over I2S). Return false to stop speaking early. */
typedef bool (*tiny_watch_pcm_fn)(void *user, const int16_t *samples, size_t count,
                                  int sample_rate_hz);

typedef struct {
    /* Where to find TINY.BIN. Tried in order:
     *   1. blob != NULL              (already in memory / rodata)
     *   2. partition_label != NULL   (raw flash partition, mmap'd while loading)
     *   3. file_path != NULL         (LittleFS / SPIFFS / SD card)
     *   4. built-in embedded TINY.BIN, if CONFIG_TINY_EMBED_MODEL is on. */
    const void *blob;
    size_t      blob_len;
    const char *partition_label;      /* default: "tiny_model"                 */
    const char *file_path;            /* default: "/storage/TINY.BIN"          */

    tiny_load_cfg_t load;             /* SRAM / PSRAM split for weights        */

    int      worker_core;             /* 0, 1, or -1 (no affinity). Default: 1 */
    int      worker_priority;         /* default: 4 (below LVGL / audio)       */
    size_t   worker_stack;            /* default: 6144                         */
    bool     acquire_pm_lock;         /* hold ESP_PM_CPU_FREQ_MAX while busy   */

    bool              speak_answers;  /* synthesise the robot voice after ans. */
    int               sample_rate_hz; /* default: 16000                        */
    tiny_watch_pcm_fn on_pcm;
    void             *pcm_user;
} tiny_watch_cfg_t;

typedef struct {
    const char         *question;     /* cleaned automatically                 */
    int                 creativity;   /* 0..9, or -1 for the service default   */
    tiny_kernel_t       kernel;
    tiny_watch_token_fn on_token;
    tiny_watch_done_fn  on_done;
    void               *user;
} tiny_watch_req_t;

tiny_watch_cfg_t tiny_watch_cfg_default(void);

tiny_err_t tiny_watch_start(const tiny_watch_cfg_t *cfg, tiny_watch_t **out);
void       tiny_watch_stop(tiny_watch_t *w);

/* Non-blocking: queues a question for the worker task. Copies `question`,
 * so the caller's buffer does not need to stay alive. */
tiny_err_t tiny_watch_ask_async(tiny_watch_t *w, const tiny_watch_req_t *req);

/* Blocking convenience wrapper (Safe to call from any task except the worker). */
tiny_err_t tiny_watch_ask(tiny_watch_t *w, const char *question, int creativity,
                          char *out, size_t out_size, tiny_stats_t *stats);

/* Abort the question currently being answered. */
void       tiny_watch_cancel(tiny_watch_t *w);
bool       tiny_watch_is_busy(const tiny_watch_t *w);

void       tiny_watch_set_creativity(tiny_watch_t *w, int creativity);
void       tiny_watch_set_voice(tiny_watch_t *w, bool enabled);
void       tiny_watch_get_model_info(const tiny_watch_t *w, tiny_model_info_t *info);

/* Stand-alone robot voice: turns `text` into 16-bit mono PCM chunks and
 * feeds them to `out`. Matches the formant buzz / hiss / click voice of the
 * original DOS program, smoothed so it sounds pleasant on a tiny speaker. */
void       tiny_voice_speak(const char *text, int sample_rate_hz,
                            tiny_watch_pcm_fn out, void *user,
                            volatile const bool *cancel);

#ifdef __cplusplus
}
#endif
