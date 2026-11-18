/* tiny_watch_ui.c - LVGL v8/v9 watch face for TINY.
 *
 * Notice that tiny_watch runs on core 1 and calls on_token / on_done from
 * its own task, while LVGL is not thread-safe. The callbacks therefore write
 * into a small lockless state and an lv_timer on the LVGL thread paints the
 * new letters at 30 Hz. No LVGL lock convention is assumed.
 */
#include "tiny_watch_ui.h"

#if __has_include("lvgl.h")
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

static struct {
    tiny_watch_t *watch;
    lv_obj_t *q_lbl, *a_lbl, *stat_lbl, *kb, *ta;
    lv_obj_t *bars[4];
    portMUX_TYPE mux;
    char q_buf[TINY_MAX_Q + 1];
    char a_buf[TINY_CTX + 1];
    int  a_len;
    float conf, lps, act[4], act_max[4];
    bool dirty, done;
} ui = { .mux = portMUX_INITIALIZER_UNLOCKED };

static const char *const presets[] = {
    "what is the capital of peru",
    "tell me a joke",
    "is a whale a fish",
    "what is 7 times 8",
    "how many moons does mars have",
    "how do you say thank you in spanish",
};

static void on_token(void *u, char c, const tiny_token_info_t *info)
{
    (void)u;
    portENTER_CRITICAL(&ui.mux);
    if (ui.a_len < TINY_CTX) {
        ui.a_buf[ui.a_len++] = c;
        ui.a_buf[ui.a_len] = 0;
    }
    ui.conf = info->confidence;
    for (int i = 0; i < 4 && i < info->layers; i++)
        ui.act[i] = info->layer_activity[i];
    ui.dirty = true;
    portEXIT_CRITICAL(&ui.mux);
}

static void on_done(void *u, tiny_err_t status, const char *answer, const tiny_stats_t *st)
{
    (void)u; (void)status; (void)answer;
    portENTER_CRITICAL(&ui.mux);
    ui.lps = st ? st->letters_per_s : 0.0f;
    ui.done = true;
    ui.dirty = true;
    portEXIT_CRITICAL(&ui.mux);
}

void tiny_watch_ui_ask(const char *question)
{
    if (!ui.watch || !question || !question[0]) return;
    tiny_watch_cancel(ui.watch);
    portENTER_CRITICAL(&ui.mux);
    tiny_clean_question(question, ui.q_buf);
    ui.a_len = 0;
    ui.a_buf[0] = 0;
    ui.done = false;
    ui.dirty = true;
    portEXIT_CRITICAL(&ui.mux);

    tiny_watch_req_t r = {
        .question = ui.q_buf,
        .creativity = -1,
        .kernel = TINY_KERNEL_PRECISE,
        .on_token = on_token,
        .on_done = on_done,
    };
    tiny_watch_ask_async(ui.watch, &r);
}

static void refresh_timer(lv_timer_t *t)
{
    (void)t;
    if (!ui.dirty) return;
    char q[TINY_MAX_Q + 1], a[TINY_CTX + 4];
    float conf, lps, act[4];
    bool done;

    portENTER_CRITICAL(&ui.mux);
    memcpy(q, ui.q_buf, sizeof q);
    memcpy(a, ui.a_buf, ui.a_len);
    int alen = ui.a_len;
    conf = ui.conf; lps = ui.lps; done = ui.done;
    memcpy(act, ui.act, sizeof act);
    ui.dirty = false;
    portEXIT_CRITICAL(&ui.mux);

    if (!done) { a[alen++] = '_'; }
    a[alen] = 0;
    lv_label_set_text(ui.q_lbl, q[0] ? q : "Tap a prompt or type a question");
    lv_label_set_text(ui.a_lbl, a[0] ? a : "...");

    char st[48];
    if (done && lps > 0.0f)
        snprintf(st, sizeof st, "TINY  %d%%  %.0f chr/s", (int)(conf * 100.0f), (double)lps);
    else
        snprintf(st, sizeof st, "TINY  %d%%", (int)(conf * 100.0f));
    lv_label_set_text(ui.stat_lbl, st);

    for (int i = 0; i < 4; i++) {
        if (act[i] > ui.act_max[i]) ui.act_max[i] = act[i];
        int v = ui.act_max[i] > 0 ? (int)(100.0f * act[i] / ui.act_max[i]) : 0;
        ui.act_max[i] *= 0.995f;
        lv_bar_set_value(ui.bars[i], v, LV_ANIM_OFF);
    }
}

#if LVGL_VERSION_MAJOR >= 9
#define EVT_OBJ(e) lv_event_get_target_obj(e)
#else
#define EVT_OBJ(e) lv_event_get_target(e)
#endif

static void chip_cb(lv_event_t *e)
{
    const char *q = (const char *)lv_event_get_user_data(e);
    tiny_watch_ui_ask(q);
}

static void ta_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        lv_obj_clear_flag(ui.kb, LV_OBJ_FLAG_HIDDEN);
    } else if (code == LV_EVENT_READY) {
        const char *txt = lv_textarea_get_text(ui.ta);
        if (txt && txt[0]) tiny_watch_ui_ask(txt);
        lv_textarea_set_text(ui.ta, "");
        lv_obj_add_flag(ui.kb, LV_OBJ_FLAG_HIDDEN);
    } else if (code == LV_EVENT_CANCEL) {
        lv_obj_add_flag(ui.kb, LV_OBJ_FLAG_HIDDEN);
    }
}

lv_obj_t *tiny_watch_ui_create(lv_obj_t *parent, tiny_watch_t *watch)
{
    ui.watch = watch;
    lv_obj_t *scr = parent ? parent : lv_scr_act();
    lv_coord_t W = lv_obj_get_width(scr), H = lv_obj_get_height(scr);
    if (W <= 0) W = 360;
    if (H <= 0) H = 360;
    /* inset for round bezels */
    lv_coord_t pad = W == H ? W / 10 : 12;

    lv_obj_set_style_bg_color(scr, lv_color_hex(0x050716), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    ui.stat_lbl = lv_label_create(scr);
    lv_obj_set_style_text_color(ui.stat_lbl, lv_color_hex(0x28E4FF), 0);
    lv_label_set_text(ui.stat_lbl, "TINY 2.1  ONLINE");
    lv_obj_align(ui.stat_lbl, LV_ALIGN_TOP_MID, 0, pad / 2 + 4);

    /* 4 tiny layer-activity bars under the header */
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = ui.bars[i] = lv_bar_create(scr);
        lv_obj_set_size(b, (W - 2 * pad - 18) / 4, 5);
        lv_bar_set_range(b, 0, 100);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x162038), LV_PART_MAIN);
        lv_obj_set_style_bg_color(b, lv_color_hex(i & 1 ? 0x28E4FF : 0x48FFA8), LV_PART_INDICATOR);
        lv_obj_align(b, LV_ALIGN_TOP_LEFT, pad + i * ((W - 2 * pad) / 4), pad / 2 + 26);
    }

    /* conversation card */
    lv_obj_t *card = lv_obj_create(scr);
    lv_obj_set_size(card, W - 2 * pad, H - 2 * pad - 86);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, pad / 2 + 38);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x0B1026), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x1C3B5E), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

    ui.q_lbl = lv_label_create(card);
    lv_obj_set_width(ui.q_lbl, lv_pct(100));
    lv_label_set_long_mode(ui.q_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(ui.q_lbl, lv_color_hex(0xFFD85A), 0);
    lv_label_set_text(ui.q_lbl, "Tap a prompt or type a question");

    ui.a_lbl = lv_label_create(card);
    lv_obj_set_width(ui.a_lbl, lv_pct(100));
    lv_label_set_long_mode(ui.a_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(ui.a_lbl, lv_color_hex(0x48FFA8), 0);
    lv_label_set_text(ui.a_lbl, "Hello! I am TINY, a neural network in your watch.");

    /* horizontal strip of quick-question chips for a small touch screen */
    lv_obj_t *strip = lv_obj_create(scr);
    lv_obj_set_size(strip, W - 2 * pad, 34);
    lv_obj_align(strip, LV_ALIGN_BOTTOM_MID, 0, -(pad / 2 + 40));
    lv_obj_set_style_bg_opa(strip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(strip, 0, 0);
    lv_obj_set_style_pad_all(strip, 0, 0);
    lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW);
    for (size_t i = 0; i < sizeof(presets) / sizeof(presets[0]); i++) {
        lv_obj_t *btn = lv_btn_create(strip);
        lv_obj_set_height(btn, 28);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x162848), 0);
        lv_obj_add_event_cb(btn, chip_cb, LV_EVENT_CLICKED, (void *)presets[i]);
        lv_obj_t *l = lv_label_create(btn);
        lv_label_set_text(l, presets[i]);
    }

    ui.ta = lv_textarea_create(scr);
    lv_textarea_set_one_line(ui.ta, true);
    lv_textarea_set_max_length(ui.ta, TINY_MAX_Q);
    lv_textarea_set_placeholder_text(ui.ta, "Ask TINY...");
    lv_obj_set_width(ui.ta, W - 2 * pad - 20);
    lv_obj_align(ui.ta, LV_ALIGN_BOTTOM_MID, 0, -(pad / 2));
    lv_obj_add_event_cb(ui.ta, ta_cb, LV_EVENT_ALL, NULL);

    ui.kb = lv_keyboard_create(scr);
    lv_keyboard_set_textarea(ui.kb, ui.ta);
    lv_obj_add_flag(ui.kb, LV_OBJ_FLAG_HIDDEN);

    if (ui.timer) lv_timer_del(ui.timer);
    ui.timer = lv_timer_create(refresh_timer, 33, NULL);
    return scr;
}
#endif
