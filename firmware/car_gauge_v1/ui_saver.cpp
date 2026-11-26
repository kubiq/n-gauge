// ============================================================================
// ui_saver.cpp  —  the screen saver: a black screen in place of gauges with
// nothing to show - the car silent - which read "--" and looked broken
// (asked 2026-11-12). All black since 2026-11-26 (asked: the dimmed N badge
// it had did not read as off): on an AMOLED every pixel off, and nothing
// drawn or redrawn. A touch anywhere still brings the gauges back. Shown and
// taken away by saverTask in the sketch. See ui_priv.h.
// ============================================================================
#include "ui_priv.h"

static volatile bool saverTouched = false;

static void saver_touch_cb(lv_event_t *e) {
  (void)e;
  saverTouched = true;                   // acted on from loop(), not from LVGL
}

lv_obj_t *ui_build_saver() {
  lv_obj_t *s = make_screen();
  // Pure black, not the screens' COL_BG (0x0d1116): that dark blue-grey
  // lights every pixel faintly, and showed in a dark car.
  lv_obj_set_style_bg_color(s, lv_color_hex(0x000000), 0);
  lv_obj_add_event_cb(s, saver_touch_cb, LV_EVENT_PRESSED, NULL);
  return s;
}

bool ui_take_saver_touch() {
  const bool t = saverTouched;
  saverTouched = false;
  return t;
}
