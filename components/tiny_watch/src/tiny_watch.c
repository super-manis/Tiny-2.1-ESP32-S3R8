/* tiny_watch.c - FreeRTOS worker, power management, and weight loading. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "spi_flash_mmap.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "tiny_watch.h"

static const char *TAG = "tiny_watch";

#if CONFIG_TINY_EMBED_MODEL
extern const uint8_t tiny_bin_start[] asm("_binary_TINY_BIN_start");
extern const uint8_t tiny_bin_end[]   asm("_binary_TINY_BIN_end");
#endif

typedef struct {
    bool                 quit;
    char                 question[TINY_MAX_Q + 1];
    int                  creativity;
    tiny_kernel_t        kernel;
    tiny_watch_token_fn  on_token;
    tiny_watch_done_fn   on_done;
    void                *user;
} job_t;

struct tiny_watch {
    tiny_model_t     *model;
    tiny_ctx_t       *ctx;
    TaskHandle_t      task;
    QueueHandle_t     queue;
    SemaphoreHandle_t exit_sem;       /* given once, when the worker exits */
    esp_pm_lock_handle_t pm_lock;
    esp_partition_mmap_handle_t map;  /* kept while tables run in place    */
    bool              mapped;

    volatile bool     busy;
    volatile bool     cancel;
    volatile bool     quitting;
    int               creativity;
    bool              speak;
    int               sample_rate_hz;
    tiny_watch_pcm_fn on_pcm;
    void             *pcm_user;
};

/* Determine the byte length of a TNY2 stream from its 12-byte header so a
 * raw flash partition (which is padded to 64 KB) can be passed directly. */
static size_t tny2_expected_size(const uint8_t *h, size_t max_len)
{
    if (max_len < 12 || memcmp(h, "TNY2", 4) != 0)
        return 0;
    int D = h[4] | (h[5] << 8);
    int L = h[6] | (h[7] << 8);
    int HID = h[10] | (h[11] << 8);
    return 12 + (size_t)TINY_VOCAB * (4 + D) + (size_t)TINY_CTX * (4 + D) +
           (size_t)L * (2 * 4 * D + 4 * (size_t)D * (4 + D) +
                        (size_t)HID * (4 + D) + (size_t)D * (4 + HID)) +
           4 * (size_t)D;
}

static tiny_err_t try_load_partition(tiny_watch_t *w, const char *label,
                                     const tiny_load_cfg_t *lcfg)
{
    if (!label || !label[0]) return TINY_ERR_FORMAT;
    const esp_partition_t *p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
    if (!p) return TINY_ERR_FORMAT;
    const void *map = NULL;
    esp_partition_mmap_handle_t mh;
    if (esp_partition_mmap(p, 0, p->size, ESP_PARTITION_MMAP_DATA, &map, &mh) != ESP_OK)
        return TINY_ERR_FORMAT;
    size_t sz = tny2_expected_size((const uint8_t *)map, p->size);
    tiny_err_t e = sz && sz <= p->size ? tiny_model_load(map, sz, lcfg, &w->model) : TINY_ERR_FORMAT;
    tiny_model_info_t mi = { 0 };
    if (e == TINY_OK)
        tiny_model_get_info(w->model, &mi);
    if (mi.weight_bytes_in_place) {
        w->map = mh;                    /* tables point into the mapping */
        w->mapped = true;
    } else {
        esp_partition_munmap(mh);
    }
    if (e == TINY_OK)
        ESP_LOGI(TAG, "loaded weights from partition '%s' (%u bytes)", label, (unsigned)sz);
    return e;
}

static tiny_err_t try_load_file(tiny_watch_t *w, const char *path, const tiny_load_cfg_t *lcfg)
{
    if (!path || !path[0]) return TINY_ERR_FORMAT;
    FILE *fp = fopen(path, "rb");
    if (!fp) return TINY_ERR_FORMAT;
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (len <= 12) { fclose(fp); return TINY_ERR_FORMAT; }
    void *buf = heap_caps_malloc((size_t)len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = malloc((size_t)len);
    if (!buf) { fclose(fp); return TINY_ERR_NOMEM; }
    size_t rd = fread(buf, 1, (size_t)len, fp);
    fclose(fp);
    /* the buffer is freed below, so tables must not point into it */
    tiny_load_cfg_t c = *lcfg;
    c.in_place = TINY_IN_PLACE_NEVER;
    tiny_err_t e = rd == (size_t)len ? tiny_model_load(buf, rd, &c, &w->model) : TINY_ERR_FORMAT;
    free(buf);
    if (e == TINY_OK)
        ESP_LOGI(TAG, "loaded weights from '%s' (%ld bytes)", path, len);
    return e;
}

typedef struct {
    const job_t  *job;
    tiny_watch_t *w;
} cb_bridge_t;

static bool on_tok_bridge(void *u, char c, const tiny_token_info_t *info)
{
    cb_bridge_t *b = (cb_bridge_t *)u;
    if (b->job->on_token)
        b->job->on_token(b->job->user, c, info);
    return !b->w->cancel;
}

static void worker_main(void *arg)
{
    tiny_watch_t *w = (tiny_watch_t *)arg;
    char answer[TINY_CTX + 1];
    job_t job;

    while (xQueueReceive(w->queue, &job, portMAX_DELAY) == pdTRUE) {
        if (job.quit) break;
        if (w->quitting) {
            /* tiny_watch_stop() is waiting: don't start new work, but still
             * complete the request so blocking callers wake up. */
            if (job.on_done) job.on_done(job.user, TINY_ERR_CANCELLED, "", NULL);
            continue;
        }
        w->cancel = false;
        w->busy = true;
        if (w->pm_lock) esp_pm_lock_acquire(w->pm_lock);

        cb_bridge_t br = { .job = &job, .w = w };
        tiny_gen_cfg_t g = tiny_gen_cfg_default();
        g.creativity = job.creativity >= 0 ? job.creativity : w->creativity;
        g.kernel = job.kernel;
        g.on_token = job.on_token ? on_tok_bridge : NULL;
        g.user = &br;
        g.cancel = &w->cancel;

        tiny_stats_t st;
        tiny_err_t err = tiny_generate(w->ctx, job.question, answer, sizeof answer, &g, &st);
        if (w->pm_lock) esp_pm_lock_release(w->pm_lock);

        if (job.on_done)
            job.on_done(job.user, err, answer, &st);
        if (err == TINY_OK && w->speak && w->on_pcm && answer[0] && !w->cancel)
            tiny_voice_speak(answer, w->sample_rate_hz, w->on_pcm, w->pcm_user, &w->cancel);

        w->busy = false;
    }
    xSemaphoreGive(w->exit_sem);
    vTaskDelete(NULL);
}

tiny_watch_cfg_t tiny_watch_cfg_default(void)
{
    tiny_watch_cfg_t c;
    memset(&c, 0, sizeof c);
    c.partition_label = "tiny_model";
    c.file_path = "/storage/TINY.BIN";
    c.load = tiny_load_cfg_default();
    c.worker_core = 1;
    c.worker_priority = 4;
    c.worker_stack = 6144;
    c.acquire_pm_lock = true;
    c.speak_answers = true;
    c.sample_rate_hz = 16000;
    return c;
}

tiny_err_t tiny_watch_start(const tiny_watch_cfg_t *cfg, tiny_watch_t **out)
{
    if (!out) return TINY_ERR_ARG;
    *out = NULL;
    tiny_watch_cfg_t dc = tiny_watch_cfg_default();
    if (!cfg) cfg = &dc;

    tiny_watch_t *w = (tiny_watch_t *)calloc(1, sizeof(*w));
    if (!w) return TINY_ERR_NOMEM;
    w->creativity = 5;
    w->speak = cfg->speak_answers;
    w->sample_rate_hz = cfg->sample_rate_hz > 0 ? cfg->sample_rate_hz : 16000;
    w->on_pcm = cfg->on_pcm;
    w->pcm_user = cfg->pcm_user;

    /* Partition and embedded rodata live in flash and stay mapped, so on a
     * PSRAM-less board (or when RAM is tight) they may fall back to in-place
     * tables. The caller's cfg->load.in_place is respected if explicitly set. */
    tiny_load_cfg_t flash_load = cfg->load;
    if (flash_load.in_place == TINY_IN_PLACE_NEVER)
        flash_load.in_place = TINY_IN_PLACE_FALLBACK;

    tiny_err_t e = TINY_ERR_FORMAT;
    if (cfg->blob && cfg->blob_len)
        e = tiny_model_load(cfg->blob, cfg->blob_len, &cfg->load, &w->model);
    if (e != TINY_OK)
        e = try_load_partition(w, cfg->partition_label, &flash_load);
    if (e != TINY_OK)
        e = try_load_file(w, cfg->file_path, &cfg->load);
#if CONFIG_TINY_EMBED_MODEL
    if (e != TINY_OK) {
        size_t len = (size_t)(tiny_bin_end - tiny_bin_start);
        size_t sz = tny2_expected_size(tiny_bin_start, len);
        e = tiny_model_load(tiny_bin_start, sz ? sz : len, &flash_load, &w->model);
        if (e == TINY_OK)
            ESP_LOGI(TAG, "loaded embedded TINY.BIN (%u bytes)", (unsigned)(sz ? sz : len));
    }
#endif
    if (e != TINY_OK) { tiny_watch_stop(w); return e; }

    e = tiny_ctx_create(w->model, &w->ctx);
    if (e != TINY_OK) { tiny_watch_stop(w); return e; }

    tiny_model_info_t mi;
    tiny_model_get_info(w->model, &mi);
    ESP_LOGI(TAG, "ready: %d layers, %d wide (%u KB in SRAM, %u KB in PSRAM, %u KB in flash)",
             mi.layers, mi.d_model,
             (unsigned)(mi.weight_bytes_fast / 1024),
             (unsigned)(mi.weight_bytes_bulk / 1024),
             (unsigned)(mi.weight_bytes_in_place / 1024));

#if CONFIG_PM_ENABLE
    if (cfg->acquire_pm_lock)
        esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "tiny_ai", &w->pm_lock);
#endif
    w->queue = xQueueCreate(2, sizeof(job_t));
    w->exit_sem = xSemaphoreCreateBinary();
    if (!w->queue || !w->exit_sem) {
        tiny_watch_stop(w);
        return TINY_ERR_NOMEM;
    }
    BaseType_t ok;
    if (cfg->worker_core >= 0 && cfg->worker_core < portNUM_PROCESSORS)
        ok = xTaskCreatePinnedToCore(worker_main, "tiny_ai", (uint32_t)cfg->worker_stack, w,
                                     cfg->worker_priority, &w->task, cfg->worker_core);
    else
        ok = xTaskCreate(worker_main, "tiny_ai", (uint32_t)cfg->worker_stack, w,
                         cfg->worker_priority, &w->task);
    if (ok != pdPASS) {
        tiny_watch_stop(w);
        return TINY_ERR_NOMEM;
    }
    *out = w;
    return TINY_OK;
}

void tiny_watch_stop(tiny_watch_t *w)
{
    if (!w) return;
    if (w->task) {
        w->quitting = true;
        w->cancel = true;
        job_t q = { .quit = true };
        xQueueSend(w->queue, &q, portMAX_DELAY);
        xSemaphoreTake(w->exit_sem, portMAX_DELAY);
    }
    if (w->pm_lock) esp_pm_lock_delete(w->pm_lock);
    if (w->queue) vQueueDelete(w->queue);
    if (w->exit_sem) vSemaphoreDelete(w->exit_sem);
    tiny_ctx_free(w->ctx);
    tiny_model_free(w->model);
    if (w->mapped) esp_partition_munmap(w->map);
    free(w);
}

tiny_err_t tiny_watch_ask_async(tiny_watch_t *w, const tiny_watch_req_t *req)
{
    if (!w || w->quitting || !req || !req->question) return TINY_ERR_ARG;
    job_t j;
    memset(&j, 0, sizeof j);
    if (tiny_clean_question(req->question, j.question) == 0)
        return TINY_ERR_ARG;
    j.creativity = req->creativity;
    j.kernel = req->kernel;
    j.on_token = req->on_token;
    j.on_done = req->on_done;
    j.user = req->user;
    return xQueueSend(w->queue, &j, 0) == pdTRUE ? TINY_OK : TINY_ERR_NOMEM;
}

typedef struct {
    SemaphoreHandle_t sem;
    char             *out;
    size_t            out_size;
    tiny_stats_t     *stats;
    tiny_err_t        err;
} sync_wait_t;

static void sync_done(void *u, tiny_err_t status, const char *answer, const tiny_stats_t *st)
{
    sync_wait_t *s = (sync_wait_t *)u;
    s->err = status;
    if (s->out && s->out_size) {
        strncpy(s->out, answer, s->out_size - 1);
        s->out[s->out_size - 1] = 0;
    }
    if (s->stats && st) *s->stats = *st;
    xSemaphoreGive(s->sem);
}

tiny_err_t tiny_watch_ask(tiny_watch_t *w, const char *question, int creativity,
                          char *out, size_t out_size, tiny_stats_t *stats)
{
    SemaphoreHandle_t sem = xSemaphoreCreateBinary();
    if (!sem) return TINY_ERR_NOMEM;
    sync_wait_t sw = { .sem = sem, .out = out, .out_size = out_size, .stats = stats };
    tiny_gen_cfg_t dc = tiny_gen_cfg_default();
    tiny_watch_req_t r = {
        .question = question,
        .creativity = creativity,
        .kernel = dc.kernel,
        .on_done = sync_done,
        .user = &sw,
    };
    tiny_err_t e = tiny_watch_ask_async(w, &r);
    if (e == TINY_OK) {
        xSemaphoreTake(sem, portMAX_DELAY);
        e = sw.err;
    }
    vSemaphoreDelete(sem);
    return e;
}

void tiny_watch_cancel(tiny_watch_t *w) { if (w) w->cancel = true; }
bool tiny_watch_is_busy(const tiny_watch_t *w) { return w ? w->busy : false; }
void tiny_watch_set_creativity(tiny_watch_t *w, int c) { if (w) w->creativity = c < 0 ? 0 : c > 9 ? 9 : c; }
void tiny_watch_set_voice(tiny_watch_t *w, bool on) { if (w) w->speak = on; }
void tiny_watch_get_model_info(const tiny_watch_t *w, tiny_model_info_t *info)
{
    if (w && w->model && info) tiny_model_get_info(w->model, info);
}
