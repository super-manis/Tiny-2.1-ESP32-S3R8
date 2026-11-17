/* tiny_voice.c - the TINY robot voice as 16-bit mono PCM.
 *
 * The DOS version drove the 8253 timer / PC speaker directly: vowels buzzed
 * by alternating two formant-like frequencies every 2 ms, s/f/h hissed with
 * random frequencies, p/t/k clicked, and the pitch drifted down through the
 * sentence. Here we render the exact same recipe into a small PCM buffer at
 * e.g. 16 kHz with a raised-cosine envelope so it doesn't pop on the
 * watch's speaker amp.
 */
#include <stdint.h>
#include <string.h>
#include "tiny_watch.h"

#define CHUNK 256
#define AMP   7000

typedef struct {
    int sr;
    uint32_t ph1, ph2, lfsr;
    tiny_watch_pcm_fn out;
    void *user;
    volatile const bool *cancel;
    int stopped;
} voice_t;

static void emit(voice_t *v, int f1, int f2, int noise, int ms)
{
    if (v->stopped) return;
    int total = (v->sr * ms) / 1000;
    int fade = v->sr / 500;                     /* 2 ms click-free edges */
    if (fade * 2 > total) fade = total / 2;
    int two_ms = (v->sr * 2) / 1000;
    if (two_ms < 1) two_ms = 1;
    uint32_t inc1 = (uint32_t)(((uint64_t)f1 << 32) / (uint32_t)v->sr);
    uint32_t inc2 = (uint32_t)(((uint64_t)f2 << 32) / (uint32_t)v->sr);
    int16_t buf[CHUNK];

    for (int pos = 0; pos < total; ) {
        if (v->cancel && *v->cancel) { v->stopped = 1; return; }
        int n = total - pos < CHUNK ? total - pos : CHUNK;
        for (int i = 0; i < n; i++, pos++) {
            int32_t s = 0;
            if (noise) {
                v->lfsr = (v->lfsr >> 1) ^ (-(v->lfsr & 1u) & 0xB400u);
                s = (v->lfsr & 1u) ? AMP / 2 : -AMP / 2;
            } else if (f1 > 0) {
                /* alternate the two formants every 2 ms, plus a softer sum */
                uint32_t inc = (pos / two_ms) & 1 ? inc2 : inc1;
                v->ph1 += inc;
                v->ph2 += (inc1 + inc2) >> 1;
                int32_t w1 = (v->ph1 & 0x80000000u) ? AMP : -AMP;
                int32_t w2 = (int32_t)(v->ph2 >> 16) - 32768;
                s = (w1 * 3 + (w2 * AMP >> 15)) >> 2;
            }
            if (fade > 0) {
                if (pos < fade) s = s * pos / fade;
                else if (total - 1 - pos < fade) s = s * (total - 1 - pos) / fade;
            }
            buf[i] = (int16_t)s;
        }
        if (!v->out(v->user, buf, (size_t)n, v->sr)) {
            v->stopped = 1;
            return;
        }
    }
}

static void say_letter(voice_t *v, int c, int shift)
{
    if (c >= 'A' && c <= 'Z') c += 32;
    #define B(f1, f2, ms) emit(v, (f1) * shift / 100, (f2) * shift / 100, 0, ms)
    switch (c) {
    case 'a': B(730, 1090, 64); break;
    case 'e': B(530, 1840, 56); break;
    case 'i': case 'y': B(300, 2290, 52); break;
    case 'o': B(570, 840, 64); break;
    case 'u': B(320, 870, 56); break;
    case 's': case 'z': case 'c': case 'x': emit(v, 0, 0, 1, 46); break;
    case 'f': case 'h': case 'v': case 'j': emit(v, 0, 0, 1, 30); break;
    case 'p': case 't': case 'k': case 'q': emit(v, 3200, 3200, 0, 3); emit(v, 0, 0, 0, 18); break;
    case 'b': case 'd': case 'g': B(180, 260, 12); emit(v, 0, 0, 0, 10); break;
    case 'm': case 'n': B(210, 300, 40); break;
    case 'l': case 'r': case 'w': B(400, 1100, 36); break;
    case ' ': emit(v, 0, 0, 0, 45); break;
    case ',': case '-': emit(v, 0, 0, 0, 110); break;
    case '.': case '!': case '?': case ':': emit(v, 0, 0, 0, 200); break;
    default:
        if (c >= '0' && c <= '9')
            B(420 + (c - '0') * 60, 900 + (c - '0') * 90, 70);
        else
            emit(v, 0, 0, 0, 20);
    }
    #undef B
}

void tiny_voice_speak(const char *text, int sample_rate_hz,
                      tiny_watch_pcm_fn out, void *user,
                      volatile const bool *cancel)
{
    if (!text || !out) return;
    voice_t v = {
        .sr = sample_rate_hz > 0 ? sample_rate_hz : 16000,
        .lfsr = 0xACE1u,
        .out = out,
        .user = user,
        .cancel = cancel,
    };
    int n = (int)strlen(text);
    for (int i = 0; i < n && !v.stopped; i++) {
        int shift = 115 - 30 * i / (n + 1);
        say_letter(&v, (unsigned char)text[i], shift);
    }
}
