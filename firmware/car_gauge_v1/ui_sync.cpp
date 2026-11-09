// ============================================================================
// ui_sync.cpp  —  the sync screen: "Syncing with the phone", in the splash's
// look, while the phone pulls the logs from the gauge (asked 2026-10-20).
// Shown and taken away by syncTask in the sketch; static but for its count,
// so nothing else redraws meanwhile. A warning still shows over it - the
// notice rim is on the top layer - and a touch brings the gauges back.
// See ui_priv.h.
// ============================================================================
#include "ui_priv.h"

extern "C" const lv_image_dsc_t n_logo;   // the splash's N badge (n_logo_img.c)

static lv_obj_t *syncCount;
static volatile bool syncTouched = false;

static void sync_touch_cb(lv_event_t *e) {
  (void)e;
  syncTouched = true;                    // acted on from loop(), not from LVGL
}

lv_obj_t *ui_build_sync() {
  lv_obj_t *s = make_screen();
  // The splash's frame: both rings, full and still.
  lv_obj_t *outer = make_arc_sized(s, COL_OIL, 0, 100, RING_OUTER_PX, RING_OUTER_W);
  lv_obj_t *inner = make_arc_sized(s, COL_BLUE, 0, 100, RING_INNER_PX, RING_INNER_W);
  lv_arc_set_value(outer, 100);
  lv_arc_set_value(inner, 100);
  lv_obj_t *badge = lv_image_create(s);
  lv_image_set_src(badge, &n_logo);
  lv_obj_align(badge, LV_ALIGN_CENTER, 0, -40);
  make_name(s, "Syncing with the phone", 50);
  syncCount = make_name(s, "", 80);
  lv_obj_set_style_text_color(syncCount, COL_DIM, 0);
  lv_obj_add_event_cb(s, sync_touch_cb, LV_EVENT_PRESSED, NULL);
  return s;
}

void ui_update_sync(uint16_t files, uint32_t bytes) {
  static int lastFiles = -1;
  static uint32_t lastKb = UINT32_MAX;
  const uint32_t kb = bytes / 1024;
  if (files == lastFiles && kb / 100 == lastKb / 100) return;   // a redraw each 0.1 MB
  lastFiles = files;
  lastKb = kb;
  if (kb < 1024) lv_label_set_text_fmt(syncCount, "%u files, %lu KB", files, (unsigned long)kb);
  else {
    const uint32_t mb10 = kb * 10 / 1024;   // no floats in LVGL's printf
    lv_label_set_text_fmt(syncCount, "%u files, %lu.%lu MB", files,
                          (unsigned long)(mb10 / 10), (unsigned long)(mb10 % 10));
  }
}

bool ui_take_sync_touch() {
  const bool t = syncTouched;
  syncTouched = false;
  return t;
}
