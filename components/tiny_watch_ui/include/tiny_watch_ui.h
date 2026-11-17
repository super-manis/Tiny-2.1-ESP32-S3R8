/*
 * tiny_watch_ui.h - Optional LVGL screen for a Waveshare ESP32-S3 watch.
 *
 * Only compiled when LVGL (v8 or v9) is present in the firmware build; the
 * engine and watch service have no UI dependency.
 *
 * Layout (sized at run time to the active display, round 360x360 or rect):
 *   - top arc / pill: layer activity bars + confidence badge + speed
 *   - middle: scrolling question / streaming answer with a glowing newest
 *     letter (a nod to the DOS neon monitor)
 *   - bottom: quick-question chips and a slide-up keyboard on tap
 */
#pragma once

#include "tiny_watch.h"

#ifdef __cplusplus
extern "C" {
#endif

#if __has_include("lvgl.h")
#include "lvgl.h"

/* Create the TINY screen on `parent` (NULL = lv_scr_act()) bound to `watch`. */
lv_obj_t *tiny_watch_ui_create(lv_obj_t *parent, tiny_watch_t *watch);

/* Ask a question from code (e.g. a physical button, gesture, or STT hook). */
void      tiny_watch_ui_ask(const char *question);
#endif

#ifdef __cplusplus
}
#endif
