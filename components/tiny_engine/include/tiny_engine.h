/*
 * tiny_engine.h - TINY 2.1 inference engine (ESP32-S3 port)
 *
 * A letter-by-letter question answering transformer (4 layers, 96 wide,
 * ~367k int8 weights).  This header is platform neutral: the same code is
 * built for the ESP32-S3 (ESP-IDF component) and for a Linux host (tests).
 *
 * Typical use:
 *
 *     tiny_model_t *m;
 *     tiny_ctx_t   *c;
 *     tiny_model_load(blob, blob_len, NULL, &m);
 *     tiny_ctx_create(m, &c);
 *     tiny_generate(c, "what is the capital of peru", out, sizeof out, &gen, &stats);
 *
 * Thread safety: a tiny_model_t is read-only after loading and may be shared.
 * A tiny_ctx_t (KV cache + scratch) must be used by one task at a time.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TINY_VOCAB      97   /* 0 pad, 1 newline, 2.. ASCII ' '..'~'      */
#define TINY_CTX        128  /* question + answer, in letters             */
#define TINY_MAX_Q      72   /* longest question, in letters              */
#define TINY_MAX_D      128
#define TINY_MAX_HID    512
#define TINY_MAX_LAYERS 8

typedef enum {
    TINY_OK = 0,
    TINY_ERR_ARG,
    TINY_ERR_FORMAT,     /* not a TNY2 file / damaged / wrong size      */
    TINY_ERR_SIZE,       /* network shape not supported                 */
    TINY_ERR_NOMEM,
    TINY_ERR_CANCELLED,
} tiny_err_t;

/* Numeric kernels. All use the int8 weights unchanged.
 *  TINY_KERNEL_PRECISE - default. Each activation vector is split into an
 *                        int8 "high" part and an int8 residual (~15 bit
 *                        precision); both are multiplied with the weights on
 *                        the ESP32-S3 PIE SIMD unit (16 MACs/instruction).
 *                        Attention scores in float. Gives the same answer as
 *                        the DOS program for 99.5% of the test questions.
 *  TINY_KERNEL_FAST    - single int8 activation pass, int8 attention.
 *                        Least arithmetic; ~93% identical answers (the rest
 *                        are mostly re-wordings).
 *  TINY_KERNEL_FLOAT   - bit-for-bit the arithmetic of the original DOS
 *                        build (float activations). Reference / testing. */
typedef enum {
    TINY_KERNEL_PRECISE = 0,
    TINY_KERNEL_FAST = 1,
    TINY_KERNEL_FLOAT = 2,
} tiny_kernel_t;

/* Whether weight tables may be used directly from the TNY2 blob (e.g.
 * memory-mapped flash) instead of being copied to RAM. Only tables whose
 * width is a multiple of 16 qualify (all of them for the stock model). */
typedef enum {
    TINY_IN_PLACE_NEVER = 0,   /* always copy; TINY_ERR_NOMEM if RAM runs out */
    TINY_IN_PLACE_FALLBACK,    /* copy, but use the blob if RAM runs out      */
    TINY_IN_PLACE_ALWAYS,      /* never copy (PSRAM-less boards, testing)     */
} tiny_in_place_t;

typedef struct {
    /* Bytes of fast internal SRAM the weights may occupy. Remaining weight
     * tables go to bulk memory (PSRAM on the watch). 0 = everything in bulk.
     * SIZE_MAX = as much as possible. Tables are placed in file order. */
    size_t fast_weight_budget;
    /* Keep at least this much internal heap free for the OS when placing
     * weights / KV cache in fast memory (ignored on the host). */
    size_t fast_reserve;
    /* Anything but NEVER requires the blob to stay valid (mapped, not
     * freed) until tiny_model_free(). */
    tiny_in_place_t in_place;
} tiny_load_cfg_t;

typedef struct tiny_model tiny_model_t;
typedef struct tiny_ctx   tiny_ctx_t;

typedef struct {
    int d_model, layers, heads, hidden;
    size_t params;
    size_t weight_bytes_fast;   /* weights placed in internal SRAM          */
    size_t weight_bytes_bulk;   /* weights placed in PSRAM / bulk memory    */
    size_t weight_bytes_in_place; /* weights read directly from the blob    */
} tiny_model_info_t;

/* Information delivered with every generated letter (for the UI). */
typedef struct {
    int   position;                       /* index in the answer           */
    float confidence;                     /* 0..1 prob. of chosen letter    */
    int   layers;
    float layer_activity[TINY_MAX_LAYERS];/* mean ReLU output per layer     */
} tiny_token_info_t;

/* Return false to stop generation early. */
typedef bool (*tiny_token_cb_t)(void *user, char letter, const tiny_token_info_t *info);

typedef struct {
    int creativity;            /* 0 = always the surest letter .. 9 = wild    */
    uint32_t seed;             /* 0 = keep the context's running RNG          */
    tiny_kernel_t kernel;
    tiny_token_cb_t on_token;  /* may be NULL                                 */
    void *user;
    volatile const bool *cancel; /* optional: set *cancel=true to abort       */
} tiny_gen_cfg_t;

typedef struct {
    int      question_letters;
    int      answer_letters;
    uint32_t prefill_us;       /* reading the question                        */
    uint32_t decode_us;        /* writing the answer                          */
    float    letters_per_s;    /* (question + answer) / total time            */
    float    mean_confidence;
} tiny_stats_t;

/* On ESP-IDF: CONFIG_TINY_FAST_WEIGHT_BUDGET_KB / CONFIG_TINY_FAST_RESERVE_KB,
 * in_place = NEVER. On the host: everything in RAM. */
tiny_load_cfg_t tiny_load_cfg_default(void);

/* Parse a TNY2 network (the original TINY.BIN) from memory. The blob may be
 * memory-mapped flash. With cfg->in_place == TINY_IN_PLACE_NEVER it is not
 * referenced after this call returns; otherwise it must outlive the model. */
tiny_err_t tiny_model_load(const void *blob, size_t len, const tiny_load_cfg_t *cfg,
                           tiny_model_t **out);
void       tiny_model_free(tiny_model_t *m);
void       tiny_model_get_info(const tiny_model_t *m, tiny_model_info_t *info);

tiny_err_t tiny_ctx_create(const tiny_model_t *m, tiny_ctx_t **out);
void       tiny_ctx_free(tiny_ctx_t *c);

/* Answer a question. `question` should already be cleaned (see
 * tiny_clean_question). Writes a NUL terminated answer to out. */
tiny_err_t tiny_generate(tiny_ctx_t *c, const char *question, char *out, size_t out_size,
                         const tiny_gen_cfg_t *cfg, tiny_stats_t *stats);

tiny_gen_cfg_t tiny_gen_cfg_default(void);

/* Tidy user input the way the network was trained: printable ASCII only,
 * max TINY_MAX_Q letters, no trailing blanks, ALL CAPS -> lower case.
 * q must hold TINY_MAX_Q + 1 bytes. Returns the length. */
int  tiny_clean_question(const char *line, char *q);

const char *tiny_err_str(tiny_err_t e);

#ifdef __cplusplus
}
#endif
