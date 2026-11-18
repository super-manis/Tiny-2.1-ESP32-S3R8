/* tiny_host.c - command line TINY for a PC, used to test the engine.
 *
 *   tiny_host [-m TINY.BIN] [-c 0..9] [--float|--fast] [--seed N] [--in-place] [--stats] < questions
 *
 * --in-place runs the weight tables straight from the file image (as on a
 * PSRAM-less board) instead of repacked copies; answers must not change.
 *
 * Reads one question per line and prints one answer per line (no prompt),
 * so outputs of different builds / kernels can be diffed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tiny_engine.h"

int main(int argc, char **argv)
{
    const char *path = "model/TINY.BIN";
    tiny_gen_cfg_t g = tiny_gen_cfg_default();
    int show_stats = 0;
    tiny_load_cfg_t lc = tiny_load_cfg_default();
    g.creativity = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-m") && i + 1 < argc) path = argv[++i];
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) g.creativity = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--float")) g.kernel = TINY_KERNEL_FLOAT;
        else if (!strcmp(argv[i], "--fast")) g.kernel = TINY_KERNEL_FAST;
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) g.seed = (uint32_t)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--stats")) show_stats = 1;
        else if (!strcmp(argv[i], "--in-place")) lc.in_place = TINY_IN_PLACE_ALWAYS;
        else { fprintf(stderr, "usage: %s [-m TINY.BIN] [-c N] [--float|--fast] [--seed N] [--in-place] [--stats]\n", argv[0]); return 2; }
    }
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror(path); return 1; }
    static unsigned char blob[1 << 20];
    size_t len = fread(blob, 1, sizeof blob, fp);
    fclose(fp);

    tiny_model_t *m;
    tiny_ctx_t *c;
    tiny_err_t e = tiny_model_load(blob, len, &lc, &m);
    if (e) { fprintf(stderr, "load: %s\n", tiny_err_str(e)); return 1; }
    if ((e = tiny_ctx_create(m, &c))) { fprintf(stderr, "ctx: %s\n", tiny_err_str(e)); return 1; }

    char line[512], q[TINY_MAX_Q + 1], out[TINY_CTX + 1];
    double total_us = 0; long total_letters = 0;
    while (fgets(line, sizeof line, stdin)) {
        if (tiny_clean_question(line, q) == 0) continue;
        tiny_stats_t st;
        tiny_generate(c, q, out, sizeof out, &g, &st);
        printf("%s\n", out);
        total_us += st.prefill_us + st.decode_us;
        total_letters += st.question_letters + 1 + st.answer_letters;
    }
    if (show_stats)
        fprintf(stderr, "%ld letters in %.1f ms (%.0f letters/s)\n", total_letters, total_us / 1000,
                total_letters * 1e6 / (total_us > 0 ? total_us : 1));
    tiny_ctx_free(c);
    tiny_model_free(m);
    return 0;
}
