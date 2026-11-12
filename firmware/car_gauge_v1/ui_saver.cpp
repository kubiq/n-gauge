// ============================================================================
// ui_saver.cpp  —  the screen saver: the N badge alone, dimmed, in place of
// gauges with nothing to show - the car silent - which read "--" and looked
// broken (asked 2026-11-12). Still: drawn once, nothing moves or redraws -
// not the boot splash, whose arcs sweep. Dim, as an AMOLED left on wants:
// almost every pixel off. Shown and taken away by saverTask in the sketch.
// See ui_priv.h.
// ============================================================================
#include "ui_priv.h"

extern "C" const lv_image_dsc_t n_logo;   // the splash's N badge (n_logo_img.c)

static volatile bool saverTouched = false;

static void saver_touch_cb(lv_event_t *e) {
  (void)e;
  saverTouched = true;                   // acted on from loop(), not from LVGL
}

lv_obj_t *ui_build_saver() {
  lv_obj_t *s = make_screen();
  lv_obj_t *badge = lv_image_create(s);
  lv_image_set_src(badge, &n_logo);
  lv_obj_set_style_image_opa(badge, SAVER_OPA, 0);
  lv_obj_center(badge);
  lv_obj_add_event_cb(s, saver_touch_cb, LV_EVENT_PRESSED, NULL);
  return s;
}

bool ui_take_saver_touch() {
  const bool t = saverTouched;
  saverTouched = false;
  return t;
}
