/* tiny_engine.c - TINY 2.1 transformer, rewritten for the ESP32-S3.
 *
 * Same network and file format as the DOS version (TINY.BIN, "TNY2"), with
 * the following changes for a microcontroller:
 *
 *  - Weights are repacked at load time into 16-byte aligned rows with the
 *    per-row scales split off, so the PIE SIMD unit can stream them.
 *  - Weight tables are placed individually in internal SRAM (fast) up to a
 *    budget, the rest in PSRAM (bulk). Small tables -> fewer fragmentation
 *    problems on a busy watch heap.
 *  - The question is read in blocks of TINY_PREFILL_BLOCK letters: each
 *    weight row is fetched from memory once per block instead of once per
 *    letter (PSRAM bandwidth, not arithmetic, is the bottleneck).
 *  - Default numerics (TINY_KERNEL_PRECISE): every activation vector is
 *    split into an int8 part and an int8 residual and multiplied with the
 *    int8 weights by the PIE SIMD unit, int32 accumulation. A single-pass
 *    TINY_KERNEL_FAST and a float path reproducing the original DOS
 *    arithmetic (TINY_KERNEL_FLOAT) are kept as well.
 *  - No double precision in the int8 path (doubles are emulated in software
 *    on the ESP32-S3), fast exp(), and no rand()/clock() globals.
 */
#include <string.h>
#include <math.h>
#include <stdlib.h>

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#include "tiny_engine.h"
#include "tiny_port.h"
#include "tiny_kernels.h"

#ifndef TINY_PREFILL_BLOCK
#define TINY_PREFILL_BLOCK 16
#endif

#define ALIGN16(n) (((n) + 15) & ~15)
#define MAX_ALLOCS (8 + 6 * TINY_MAX_LAYERS)

typedef struct {
    const float  *scale;   /* one per row                      */
    const int8_t *w;       /* rows x stride, 16-byte aligned   */
    int rows, cols, stride;
} table_t;

struct tiny_model {
    int D, L, H, HD, HID;
    float att_div, inv_att_div;
    table_t emb, pos;
    table_t wq[TINY_MAX_LAYERS], wk[TINY_MAX_LAYERS], wv[TINY_MAX_LAYERS], wo[TINY_MAX_LAYERS];
    table_t f1[TINY_MAX_LAYERS], f2[TINY_MAX_LAYERS];
    const float *g1[TINY_MAX_LAYERS], *g2[TINY_MAX_LAYERS], *gf;
    size_t fast_reserve;
    size_t bytes_fast, bytes_bulk, params;
    void *allocs[MAX_ALLOCS];
    int nallocs;
};

struct tiny_ctx {
    const tiny_model_t *m;
    /* KV cache: per layer and position, D int8 values + one scale. */
    int8_t *kc[TINY_MAX_LAYERS], *vc[TINY_MAX_LAYERS];
    float  *ks[TINY_MAX_LAYERS], *vs[TINY_MAX_LAYERS];
    /* scratch for a block of B tokens */
    float  *x, *h, *t, *q, *a, *mid;    /* [B][D] ... mid [B][HID]     */
    int8_t *xq;                         /* [2][B][qstride] int8 + residual */
    float  *xs, *xs2;                   /* [B] their scales            */
    int     qstride;
    float   score[TINY_VOCAB];
    float   prob[TINY_VOCAB];
    float   att[TINY_CTX];
    float   layer_act[TINY_MAX_LAYERS];
    tiny_kernel_t kernel;
    uint32_t rng;
    void   *allocs[4 * TINY_MAX_LAYERS + 4];
    int     nallocs;
};

/* ------------------------------------------------------------------ */
/* small maths helpers                                                 */

/* e^x for x <= 0 (all our uses subtract the maximum first); 0 below -60
 * like the original safe_exp(). ~1e-6 relative error, no libm/double. */
static inline float fast_exp(float x)
{
    if (x < -60.0f)
        return 0.0f;
    float t = x * 1.44269504089f;
    int i = (int)t;
    if ((float)i > t) i--;
    float f = t - (float)i;
    float p = 1.0f + f * (0.693147182f + f * (0.240226507f + f * (0.0555041087f +
              f * (0.00961812911f + f * 0.00133335581f))));
    union { float f; uint32_t u; } v;
    v.u = (uint32_t)(i + 127) << 23;
    return p * v.f;
}

/* the original DOS version */
static inline float ref_exp(float x)
{
    if (x < -60.0f)
        return 0.0f;
    return (float)exp(x);
}

static inline float kexp(const tiny_ctx_t *c, float x)
{
    return c->kernel == TINY_KERNEL_FLOAT ? ref_exp(x) : fast_exp(x);
}

static inline uint32_t rng_next(tiny_ctx_t *c)
{
    uint32_t x = c->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return c->rng = x ? x : 0x9E3779B9u;
}

/* ------------------------------------------------------------------ */
/* loading                                                             */

typedef struct {
    const uint8_t *p, *end;
    int bad;
} reader_t;

static int rd16(reader_t *r)
{
    if (r->end - r->p < 2) { r->bad = 1; return 0; }
    int v = r->p[0] | (r->p[1] << 8);
    r->p += 2;
    return v;
}

static float rdf(reader_t *r)
{
    float f = 0.0f;
    if (r->end - r->p < 4) { r->bad = 1; return 0.0f; }
    memcpy(&f, r->p, 4);
    r->p += 4;
    return f;
}

static void *model_alloc(tiny_model_t *m, size_t size, int prefer_fast, size_t *budget)
{
    void *p = NULL;
    if (m->nallocs >= MAX_ALLOCS)
        return NULL;
    size = ALIGN16(size);
    if (prefer_fast && size <= *budget) {
        size_t freeb = tiny_port_fast_free();
        /* If there is no PSRAM, save ~160 KB of internal RAM for the KV cache
         * and prefill scratch; remaining weight tables run in-place from flash. */
        size_t keep = m->fast_reserve;
        void *probe = tiny_port_alloc(16, TINY_MEM_BULK);
        if (probe) tiny_port_free(probe);
        else keep += 160 * 1024;
        if (freeb != SIZE_MAX ? (freeb > size + keep && tiny_port_fast_largest() >= size) : 1)
            p = tiny_port_alloc(size, TINY_MEM_FAST);
        if (p) {
            *budget -= size;
            m->bytes_fast += size;
        }
    }
    if (!p) {
        p = tiny_port_alloc(size, TINY_MEM_BULK);
        if (p) m->bytes_bulk += size;
    }
    if (p)
        m->allocs[m->nallocs++] = p;
    return p;
}

static tiny_err_t load_table(tiny_model_t *m, reader_t *r, table_t *t, int rows, int cols,
                             float **scales, int prefer_fast, size_t *budget)
{
    const uint8_t *raw = r->p;
    int stride = ALIGN16(cols);
    int8_t *w = (int8_t *)model_alloc(m, (size_t)rows * stride, prefer_fast, budget);
    float *sc = *scales;
    *scales += rows;
    for (int i = 0; i < rows; i++) {
        sc[i] = rdf(r);
        if (r->end - r->p < cols) { r->bad = 1; return TINY_ERR_FORMAT; }
        if (w) {
            memcpy(w + (size_t)i * stride, r->p, cols);
            if (stride > cols)
                memset(w + (size_t)i * stride + cols, 0, stride - cols);
        }
        r->p += cols;
    }
    if (w) {
        t->w = w;
        t->stride = stride;
    } else if (cols % 16 == 0) {
        /* Zero-copy fallback (e.g. PSRAM-less board or emulator with embedded
         * flash rodata): point directly into the TNY2 stream. */
        t->w = (const int8_t *)(raw + 4);
        t->stride = 4 + cols;
    } else {
        return TINY_ERR_NOMEM;
    }
    t->scale = sc;
    t->rows = rows;
    t->cols = cols;
    m->params += (size_t)rows * cols;
    return r->bad ? TINY_ERR_FORMAT : TINY_OK;
}

static tiny_err_t load_gains(tiny_model_t *m, reader_t *r, const float **g, float **scales)
{
    float *dst = *scales;
    *scales += m->D;
    for (int i = 0; i < m->D; i++)
        dst[i] = rdf(r);
    *g = dst;
    m->params += m->D;
    return r->bad ? TINY_ERR_FORMAT : TINY_OK;
}

tiny_load_cfg_t tiny_load_cfg_default(void)
{
    tiny_load_cfg_t c = { .fast_weight_budget = SIZE_MAX, .fast_reserve = 64 * 1024 };
    return c;
}

tiny_err_t tiny_model_load(const void *blob, size_t len, const tiny_load_cfg_t *cfg,
                           tiny_model_t **out)
{
    if (!blob || !out)
        return TINY_ERR_ARG;
    *out = NULL;
    tiny_load_cfg_t dc = tiny_load_cfg_default();
    if (!cfg) cfg = &dc;

    reader_t r = { (const uint8_t *)blob, (const uint8_t *)blob + len, 0 };
    if (len < 12 || memcmp(r.p, "TNY2", 4) != 0)
        return TINY_ERR_FORMAT;
    r.p += 4;

    tiny_model_t *m = (tiny_model_t *)calloc(1, sizeof(*m));
    if (!m)
        return TINY_ERR_NOMEM;
    m->D = rd16(&r); m->L = rd16(&r); m->H = rd16(&r); m->HID = rd16(&r);
    tiny_err_t err = TINY_OK;
    if (m->D < 8 || m->D > TINY_MAX_D || m->L < 1 || m->L > TINY_MAX_LAYERS || m->H < 1 ||
        m->D % m->H != 0 || m->HID < 8 || m->HID > TINY_MAX_HID) {
        err = TINY_ERR_SIZE;
        goto fail;
    }
    m->HD = m->D / m->H;
    m->att_div = (float)sqrt((double)m->HD);
    m->inv_att_div = 1.0f / m->att_div;
    m->fast_reserve = cfg->fast_reserve;

    const int D = m->D, L = m->L, HID = m->HID;
    size_t budget = cfg->fast_weight_budget;
    size_t zero = 0;

    /* All row scales and gains in one small block (always fast if possible). */
    size_t nscales = TINY_VOCAB + TINY_CTX + (size_t)L * (4 * D + HID + D) + (size_t)(2 * L + 1) * D;
    size_t big = SIZE_MAX;
    float *scales = (float *)model_alloc(m, nscales * sizeof(float), 1, &big);
    if (!scales) { err = TINY_ERR_NOMEM; goto fail; }

    /* Expected file size check before allocating megabytes of tables. */
    size_t need = 12 + (size_t)TINY_VOCAB * (4 + D) + (size_t)TINY_CTX * (4 + D) +
                  (size_t)L * (2 * 4 * D + 4 * (size_t)D * (4 + D) + (size_t)HID * (4 + D) + (size_t)D * (4 + HID)) +
                  4 * (size_t)D;
    if (len != need) { err = TINY_ERR_FORMAT; goto fail; }

    /* emb is read in full every letter (it is also the output layer);
     * pos only one row per letter, so it never needs fast memory. */
    if ((err = load_table(m, &r, &m->emb, TINY_VOCAB, D, &scales, 1, &budget))) goto fail;
    if ((err = load_table(m, &r, &m->pos, TINY_CTX, D, &scales, 0, &zero))) goto fail;
    for (int l = 0; l < L; l++) {
        if ((err = load_gains(m, &r, &m->g1[l], &scales))) goto fail;
        if ((err = load_table(m, &r, &m->wq[l], D, D, &scales, 1, &budget))) goto fail;
        if ((err = load_table(m, &r, &m->wk[l], D, D, &scales, 1, &budget))) goto fail;
        if ((err = load_table(m, &r, &m->wv[l], D, D, &scales, 1, &budget))) goto fail;
        if ((err = load_table(m, &r, &m->wo[l], D, D, &scales, 1, &budget))) goto fail;
        if ((err = load_gains(m, &r, &m->g2[l], &scales))) goto fail;
        if ((err = load_table(m, &r, &m->f1[l], HID, D, &scales, 1, &budget))) goto fail;
        if ((err = load_table(m, &r, &m->f2[l], D, HID, &scales, 1, &budget))) goto fail;
    }
    if ((err = load_gains(m, &r, &m->gf, &scales))) goto fail;
    if (r.bad || r.p != r.end) { err = TINY_ERR_FORMAT; goto fail; }
    *out = m;
    return TINY_OK;

fail:
    tiny_model_free(m);
    return err;
}

void tiny_model_free(tiny_model_t *m)
{
    if (!m) return;
    for (int i = 0; i < m->nallocs; i++)
        tiny_port_free(m->allocs[i]);
    free(m);
}

void tiny_model_get_info(const tiny_model_t *m, tiny_model_info_t *info)
{
    info->d_model = m->D;
    info->layers = m->L;
    info->heads = m->H;
    info->hidden = m->HID;
    info->params = m->params;
    info->weight_bytes_fast = m->bytes_fast;
    info->weight_bytes_bulk = m->bytes_bulk;
}

/* ------------------------------------------------------------------ */
/* context                                                             */

static void *ctx_alloc(tiny_ctx_t *c, size_t size, size_t reserve)
{
    size = ALIGN16(size);
    void *p = NULL;
    size_t freeb = tiny_port_fast_free();
    if (freeb == SIZE_MAX || (freeb > size + reserve && tiny_port_fast_largest() >= size))
        p = tiny_port_alloc(size, TINY_MEM_FAST);
    if (!p) p = tiny_port_alloc(size, TINY_MEM_BULK);
    if (!p) p = tiny_port_alloc(size, TINY_MEM_FAST);
    if (p) {
        memset(p, 0, size);
        c->allocs[c->nallocs++] = p;
    }
    return p;
}

tiny_err_t tiny_ctx_create(const tiny_model_t *m, tiny_ctx_t **out)
{
    if (!m || !out) return TINY_ERR_ARG;
    tiny_ctx_t *c = (tiny_ctx_t *)calloc(1, sizeof(*c));
    if (!c) return TINY_ERR_NOMEM;
    c->m = m;
    const int B = TINY_PREFILL_BLOCK, D = m->D, HID = m->HID;
    c->qstride = ALIGN16(D > HID ? D : HID);

    /* Scratch first: it is touched many times per weight byte. */
    float *fs = (float *)ctx_alloc(c, sizeof(float) * ((size_t)B * (5 * D + HID) + 2 * B), m->fast_reserve);
    c->xq = (int8_t *)ctx_alloc(c, (size_t)2 * B * c->qstride, m->fast_reserve);
    if (!fs || !c->xq) goto nomem;
    c->x = fs;
    c->h = c->x + B * D;
    c->t = c->h + B * D;
    c->q = c->t + B * D;
    c->a = c->q + B * D;
    c->mid = c->a + B * D;
    c->xs = c->mid + B * HID;
    c->xs2 = c->xs + B;

    for (int l = 0; l < m->L; l++) {
        /* key and value cache of one layer share one allocation */
        int8_t *kv = (int8_t *)ctx_alloc(c, (size_t)2 * TINY_CTX * D + 2 * TINY_CTX * sizeof(float),
                                         m->fast_reserve);
        if (!kv) goto nomem;
        c->kc[l] = kv;
        c->vc[l] = kv + TINY_CTX * D;
        c->ks[l] = (float *)(kv + 2 * TINY_CTX * D);
        c->vs[l] = c->ks[l] + TINY_CTX;
    }
    c->rng = tiny_port_random() | 1u;
    *out = c;
    return TINY_OK;
nomem:
    tiny_ctx_free(c);
    return TINY_ERR_NOMEM;
}

void tiny_ctx_free(tiny_ctx_t *c)
{
    if (!c) return;
    for (int i = 0; i < c->nallocs; i++)
        tiny_port_free(c->allocs[i]);
    free(c);
}

/* ------------------------------------------------------------------ */
/* the network                                                         */

/* RMS norm: y = x / rms(x) * g */
static void norm(const tiny_ctx_t *c, const float *x, const float *g, float *y)
{
    const int D = c->m->D;
    float s = 0.0f;
    for (int i = 0; i < D; i++)
        s += x[i] * x[i];
    if (c->kernel == TINY_KERNEL_FLOAT)
        s = (float)(1.0 / sqrt(s / D + 1e-5));     /* exactly as on DOS */
    else
        s = 1.0f / sqrtf(s / (float)D + 1e-5f);
    for (int i = 0; i < D; i++)
        y[i] = x[i] * s * g[i];
}

/* int8 copy of x[0..n-1], zero padded to np; returns the scale. */
static float quantize(const float *x, int n, int np, int8_t *q)
{
    float big = 0.0f;
    for (int i = 0; i < n; i++) {
        float v = fabsf(x[i]);
        if (v > big) big = v;
    }
    if (big < 1e-20f) {
        memset(q, 0, np);
        return 0.0f;
    }
    float k = 127.0f / big;
    for (int i = 0; i < n; i++) {
        float v = x[i] * k;
        q[i] = (int8_t)(v < 0 ? v - 0.5f : v + 0.5f);
    }
    for (int i = n; i < np; i++) q[i] = 0;
    return big / 127.0f;
}

/* Prepare n input vectors (stride `is`, length `cols`) for an int8 matmul.
 * PRECISE also stores the rounding residual as a second int8 vector, so
 * x ~= xs*xq + xs2*xq2 with ~15 bits of precision. */
static void quantize_block(tiny_ctx_t *c, const float *in, int is, int n, int cols)
{
    if (c->kernel == TINY_KERNEL_FLOAT) return;
    const int np = ALIGN16(cols);
    for (int t = 0; t < n; t++) {
        const float *x = in + t * is;
        int8_t *q1 = c->xq + t * c->qstride;
        float s = c->xs[t] = quantize(x, cols, np, q1);
        if (c->kernel == TINY_KERNEL_PRECISE) {
            float r[TINY_MAX_HID];
            for (int i = 0; i < cols; i++)
                r[i] = x[i] - (float)q1[i] * s;
            c->xs2[t] = quantize(r, cols, np, c->xq + (TINY_PREFILL_BLOCK + t) * c->qstride);
        }
    }
}

/* out[t][r] = table[r] . in[t]   for t < n.  Rows outer, tokens inner, so
 * every weight row is fetched from (PSRAM) memory once per block. In the
 * int8 path `in` must have been passed to quantize_block() first. */
static void matmul(tiny_ctx_t *c, const table_t *tb, const float *in, int is,
                   int n, float *out, int os)
{
    const int rows = tb->rows, cols = tb->cols, stride = tb->stride;
    const int8_t *w = tb->w;
    if (c->kernel == TINY_KERNEL_FLOAT) {
        for (int r = 0; r < rows; r++, w += stride) {
            const float sc = tb->scale[r];
            for (int t = 0; t < n; t++) {
                const float *x = in + t * is;
                float s = 0.0f;
                for (int i = 0; i < cols; i++)
                    s += w[i] * x[i];
                out[t * os + r] = s * sc;
            }
        }
        return;
    }
    const int8_t *xq = c->xq;
    const int qs = c->qstride;
    if (c->kernel == TINY_KERNEL_PRECISE) {
        const int8_t *xq2 = xq + TINY_PREFILL_BLOCK * qs;
        for (int r = 0; r < rows; r++, w += stride) {
            const float sc = tb->scale[r];
            for (int t = 0; t < n; t++)          /* row w stays in L1 cache */
                out[t * os + r] = ((float)tiny_dot_s8(w, xq + t * qs, stride) * c->xs[t] +
                                   (float)tiny_dot_s8(w, xq2 + t * qs, stride) * c->xs2[t]) * sc;
        }
        return;
    }
    for (int r = 0; r < rows; r++, w += stride) {
        const float sc = tb->scale[r];
        for (int t = 0; t < n; t++)
            out[t * os + r] = (float)tiny_dot_s8(w, xq + t * qs, stride) * (sc * c->xs[t]);
    }
}

/* keep D numbers as bytes with one scale (KV cache) - as in the original */
static float store(int D, int8_t *dst, const float *src)
{
    float big = 0.0f, v, k;
    for (int i = 0; i < D; i++) {
        v = src[i] < 0 ? -src[i] : src[i];
        if (v > big) big = v;
    }
    if (big < 1e-20f)
        big = 1e-20f;
    k = 127.0f / big;
    for (int i = 0; i < D; i++) {
        v = src[i] * k;
        dst[i] = (int8_t)(v < 0 ? v - 0.5f : v + 0.5f);
    }
    return big / 127.0f;
}

/* Causal self-attention for the token at position p; q is its query. */
static void attend(tiny_ctx_t *c, int l, int p, const float *q, float *out)
{
    const tiny_model_t *m = c->m;
    const int D = m->D, HD = m->HD;
    const int8_t *kc = c->kc[l], *vc = c->vc[l];
    const float *ks = c->ks[l], *vs = c->vs[l];
    float *bs = c->att;
    int8_t qq[TINY_MAX_D];

    for (int h = 0; h < m->H; h++) {
        const float *qh = q + h * HD;
        float big = -1e30f, sum = 0.0f;
        if (c->kernel == TINY_KERNEL_FLOAT) {
            for (int j = 0; j <= p; j++) {
                const int8_t *kp = kc + j * D + h * HD;
                float s = 0.0f;
                for (int i = 0; i < HD; i++)
                    s += qh[i] * kp[i];
                s = s * ks[j] / m->att_div;
                bs[j] = s;
                if (s > big) big = s;
            }
        } else if (c->kernel == TINY_KERNEL_PRECISE) {
            for (int j = 0; j <= p; j++) {
                const int8_t *kp = kc + j * D + h * HD;
                float s = 0.0f;
                for (int i = 0; i < HD; i++)
                    s += qh[i] * kp[i];
                s = s * (ks[j] * m->inv_att_div);
                bs[j] = s;
                if (s > big) big = s;
            }
        } else {
            float qsc = quantize(qh, HD, HD, qq) * m->inv_att_div;
            for (int j = 0; j <= p; j++) {
                float s = (float)tiny_dot_s8_any(qq, kc + j * D + h * HD, HD) * (qsc * ks[j]);
                bs[j] = s;
                if (s > big) big = s;
            }
        }
        for (int j = 0; j <= p; j++) {
            bs[j] = kexp(c, bs[j] - big);
            sum += bs[j];
        }
        float *oh = out + h * HD;
        for (int i = 0; i < HD; i++)
            oh[i] = 0.0f;
        const float inv = 1.0f / sum;
        for (int j = 0; j <= p; j++) {
            if (bs[j] == 0.0f)
                continue;
            const int8_t *vp = vc + j * D + h * HD;
            float s = c->kernel == TINY_KERNEL_FLOAT ? bs[j] * vs[j] / sum : bs[j] * vs[j] * inv;
            for (int i = 0; i < HD; i++)
                oh[i] += s * vp[i];
        }
    }
}

/* Run n tokens at positions p0.. through the network. If score != NULL the
 * next-letter scores for the last token are written there. */
static void forward(tiny_ctx_t *c, const uint8_t *tok, int n, int p0, float *score)
{
    const tiny_model_t *m = c->m;
    const int D = m->D, HID = m->HID;
    float *x = c->x, *h = c->h, *t = c->t, *q = c->q, *a = c->a, *mid = c->mid;

    for (int b = 0; b < n; b++) {
        const int8_t *e = m->emb.w + tok[b] * m->emb.stride;
        const int8_t *ps = m->pos.w + (p0 + b) * m->pos.stride;
        const float es = m->emb.scale[tok[b]], pss = m->pos.scale[p0 + b];
        for (int i = 0; i < D; i++)
            x[b * D + i] = e[i] * es + ps[i] * pss;
    }

    for (int l = 0; l < m->L; l++) {
        /* look back at the earlier letters */
        for (int b = 0; b < n; b++)
            norm(c, x + b * D, m->g1[l], h + b * D);
        quantize_block(c, h, D, n, D);
        matmul(c, &m->wk[l], h, D, n, t, D);
        for (int b = 0; b < n; b++)
            c->ks[l][p0 + b] = store(D, c->kc[l] + (p0 + b) * D, t + b * D);
        matmul(c, &m->wv[l], h, D, n, t, D);
        for (int b = 0; b < n; b++)
            c->vs[l][p0 + b] = store(D, c->vc[l] + (p0 + b) * D, t + b * D);
        matmul(c, &m->wq[l], h, D, n, q, D);
        for (int b = 0; b < n; b++)
            attend(c, l, p0 + b, q + b * D, a + b * D);
        quantize_block(c, a, D, n, D);
        matmul(c, &m->wo[l], a, D, n, t, D);
        for (int i = 0; i < n * D; i++)
            x[i] += t[i];

        /* think about it */
        for (int b = 0; b < n; b++)
            norm(c, x + b * D, m->g2[l], h + b * D);
        quantize_block(c, h, D, n, D);
        matmul(c, &m->f1[l], h, D, n, mid, HID);
        for (int b = 0; b < n; b++) {
            float s = 0.0f, *mm = mid + b * HID;
            for (int i = 0; i < HID; i++) {
                if (mm[i] < 0.0f) mm[i] = 0.0f;
                else s += mm[i];
            }
            if (b == n - 1)
                c->layer_act[l] = s / HID;
        }
        quantize_block(c, mid, HID, n, HID);
        matmul(c, &m->f2[l], mid, HID, n, t, D);
        for (int i = 0; i < n * D; i++)
            x[i] += t[i];
    }
    if (score) {
        norm(c, x + (n - 1) * D, m->gf, h);
        quantize_block(c, h, D, 1, D);
        matmul(c, &m->emb, h, D, 1, score, TINY_VOCAB);
    }
}

/* ------------------------------------------------------------------ */
/* choosing letters                                                    */

static int chosen(tiny_ctx_t *c, int tok, float *conf)
{
    float sum = 0.0f;
    for (int i = 0; i < TINY_VOCAB; i++)
        sum += kexp(c, c->score[i] - c->score[tok]);
    *conf = 1.0f / sum;
    return tok;
}

/* Creativity 0: always the best letter. Otherwise pick at random among the
 * letters that fit nearly as well (same rule as the DOS version). */
static int pick(tiny_ctx_t *c, int creativity, float *conf)
{
    const float *score = c->score;
    float *bs = c->prob;
    int best = 0;
    for (int i = 1; i < TINY_VOCAB; i++)
        if (score[i] > score[best]) best = i;
    if (creativity <= 0)
        return chosen(c, best, conf);
    float t = creativity / 5.0f, sum = 0.0f;
    for (int i = 0; i < TINY_VOCAB; i++) {
        bs[i] = kexp(c, (score[i] - score[best]) / t);
        if (bs[i] < 0.15f) bs[i] = 0.0f;        /* never pick unlikely letters */
        sum += bs[i];
    }
    float r = sum * (float)(rng_next(c) % 10000) / 10000.0f;
    for (int i = 0; i < TINY_VOCAB; i++) {
        r -= bs[i];
        if (r < 0.0f && bs[i] > 0.0f) return chosen(c, i, conf);
    }
    return chosen(c, best, conf);
}

tiny_gen_cfg_t tiny_gen_cfg_default(void)
{
    tiny_gen_cfg_t g;
    memset(&g, 0, sizeof g);
    g.creativity = 5;
#if defined(CONFIG_TINY_KERNEL_FAST)
    g.kernel = TINY_KERNEL_FAST;
#elif defined(CONFIG_TINY_KERNEL_FLOAT)
    g.kernel = TINY_KERNEL_FLOAT;
#else
    g.kernel = TINY_KERNEL_PRECISE;
#endif
    return g;
}

tiny_err_t tiny_generate(tiny_ctx_t *c, const char *question, char *out, size_t out_size,
                         const tiny_gen_cfg_t *cfg, tiny_stats_t *stats)
{
    if (!c || !question || !out || out_size == 0)
        return TINY_ERR_ARG;
    tiny_gen_cfg_t dc = tiny_gen_cfg_default();
    if (!cfg) cfg = &dc;
    c->kernel = cfg->kernel;
    if (cfg->seed) {                            /* spread small seeds out */
        uint32_t z = cfg->seed * 0x9E3779B9u;
        z = (z ^ (z >> 16)) * 0x85EBCA6Bu;
        z = (z ^ (z >> 13)) * 0xC2B2AE35u;
        c->rng = (z ^ (z >> 16)) | 1u;
    }

    uint8_t toks[TINY_MAX_Q + 1];
    int n = 0;
    for (const char *s = question; *s && n < TINY_MAX_Q; s++)
        if (*s >= 32 && *s <= 126)
            toks[n++] = (uint8_t)(*s - 30);
    const int qlen = n;
    toks[n++] = 1;                              /* end of the question */

    tiny_err_t err = TINY_OK;
    uint64_t t0 = tiny_port_time_us();

    /* read the question, a block at a time */
    for (int p = 0; p < n; p += TINY_PREFILL_BLOCK) {
        int len = n - p < TINY_PREFILL_BLOCK ? n - p : TINY_PREFILL_BLOCK;
        forward(c, toks + p, len, p, p + len == n ? c->score : NULL);
        if (cfg->cancel && *cfg->cancel) { err = TINY_ERR_CANCELLED; break; }
    }
    uint64_t t1 = tiny_port_time_us();

    /* write the answer, a letter at a time */
    int letters = 0;
    float conf_sum = 0.0f;
    tiny_token_info_t info;
    info.layers = c->m->L;
    for (int p = n; err == TINY_OK && p < TINY_CTX && (size_t)letters + 1 < out_size; p++) {
        float conf;
        int tok = pick(c, cfg->creativity, &conf);
        if (tok <= 1) break;                    /* end of the answer */
        char ch = (char)(tok + 30);
        out[letters++] = ch;
        conf_sum += conf;
        if (cfg->on_token) {
            info.position = letters - 1;
            info.confidence = conf;
            memcpy(info.layer_activity, c->layer_act, sizeof(float) * c->m->L);
            if (!cfg->on_token(cfg->user, ch, &info)) break;
        }
        if (cfg->cancel && *cfg->cancel) { err = TINY_ERR_CANCELLED; break; }
        if (p + 1 < TINY_CTX) {
            uint8_t tb = (uint8_t)tok;
            forward(c, &tb, 1, p, c->score);
        }
        tiny_port_yield();
    }
    out[letters] = 0;
    uint64_t t2 = tiny_port_time_us();

    if (stats) {
        stats->question_letters = qlen;
        stats->answer_letters = letters;
        stats->prefill_us = (uint32_t)(t1 - t0);
        stats->decode_us = (uint32_t)(t2 - t1);
        stats->letters_per_s = t2 > t0 ? (float)(qlen + 1 + letters) * 1e6f / (float)(t2 - t0) : 0.0f;
        stats->mean_confidence = letters ? conf_sum / letters : 0.0f;
    }
    return err;
}

/* ------------------------------------------------------------------ */

int tiny_clean_question(const char *line, char *q)
{
    int i, n = 0, lower = 0, upper = 0;
    for (i = 0; line[i] && n < TINY_MAX_Q; i++)
        if (line[i] >= 32 && line[i] <= 126)
            q[n++] = line[i];
    while (n > 0 && q[n - 1] == ' ')
        n--;
    q[n] = 0;
    for (i = 0; i < n; i++) {
        if (q[i] >= 'a' && q[i] <= 'z') lower++;
        if (q[i] >= 'A' && q[i] <= 'Z') upper++;
    }
    if (upper > 0 && lower == 0)
        for (i = 0; i < n; i++)
            if (q[i] >= 'A' && q[i] <= 'Z') q[i] += 'a' - 'A';
    return n;
}

const char *tiny_err_str(tiny_err_t e)
{
    switch (e) {
    case TINY_OK: return "ok";
    case TINY_ERR_ARG: return "bad argument";
    case TINY_ERR_FORMAT: return "model file is not a valid TNY2 network";
    case TINY_ERR_SIZE: return "network size not supported";
    case TINY_ERR_NOMEM: return "out of memory";
    case TINY_ERR_CANCELLED: return "cancelled";
    }
    return "?";
}
