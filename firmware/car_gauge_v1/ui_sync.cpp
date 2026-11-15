// ============================================================================
// ui_sync.cpp  —  the sync screen: "Syncing with the phone", in the splash's
// look, while the phone pulls the logs from the gauge (asked 2026-10-20).
// Shown and taken away by syncTask in the sketch; static but for its count,
// so nothing else redraws meanwhile. A warning still shows over it - the
// notice rim is on the top layer - and a touch brings the gauges back.
// See ui_priv.h.
// ============================================================================
#include "ui_priv.h"
#include <stdio.h>      // snprintf

extern "C" const lv_image_dsc_t n_logo;   // the splash's N badge (n_logo_img.c)

static lv_obj_t *syncCount, *syncOuter;
static volatile bool syncTouched = false;

static void sync_touch_cb(lv_event_t *e) {
  (void)e;
  syncTouched = true;                    // acted on from loop(), not from LVGL
}

lv_obj_t *ui_build_sync() {
  lv_obj_t *s = make_screen();
  // The splash's frame: both rings, full and still - the outer one the
  // sync's progress when the phone says its plan (ui_update_sync).
  syncOuter = make_arc_sized(s, COL_OIL, 0, 1000, RING_OUTER_PX, RING_OUTER_W);
  lv_obj_t *inner = make_arc_sized(s, COL_BLUE, 0, 100, RING_INNER_PX, RING_INNER_W);
  lv_arc_set_value(syncOuter, 1000);
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

// "3.4 MB", "820 KB" - no floats in LVGL's printf.
static void fmtSize(char *out, size_t n, uint32_t bytes) {
  const uint32_t kb = bytes / 1024;
  if (kb < 1024) snprintf(out, n, "%lu KB", (unsigned long)kb);
  else {
    const uint32_t mb10 = kb * 10 / 1024;
    snprintf(out, n, "%lu.%lu MB", (unsigned long)(mb10 / 10), (unsigned long)(mb10 % 10));
  }
}

void ui_update_sync(uint16_t files, uint32_t bytes, const UiSyncPlan *plan) {
  // A redraw each 0.1 MB, and the ring each 0.1 % - not every chunk.
  static int lastFiles = -1, lastAt = -1, lastPermille = -1;
  static uint32_t lastKb = UINT32_MAX;
  const uint32_t shownBytes = plan ? plan->done : bytes;
  const int shownFiles = plan ? plan->at : files;
  const uint32_t kb = shownBytes / 1024;
  const int permille = plan && plan->total ? (int)((uint64_t)plan->done * 1000 / plan->total) : 1000;
  if (permille != lastPermille) {
    lastPermille = permille;
    lv_arc_set_value(syncOuter, permille);
  }
  if (shownFiles == lastFiles && (plan ? plan->at : -1) == lastAt && kb / 100 == lastKb / 100) return;
  lastFiles = shownFiles;
  lastAt = plan ? plan->at : -1;
  lastKb = kb;
  char got[16];
  fmtSize(got, sizeof(got), shownBytes);
  if (plan) {
    // As the phone shows it: the file it is on, of how many; the bytes.
    char total[16];
    fmtSize(total, sizeof(total), plan->total);
    // A comma, not a dot: the small font may lack "·" (ui_overview.cpp).
    lv_label_set_text_fmt(syncCount, "file %u of %u, %s of %s", plan->at, plan->of, got, total);
  } else {
    lv_label_set_text_fmt(syncCount, "%u files, %s", files, got);
  }
}

bool ui_take_sync_touch() {
  const bool t = syncTouched;
  syncTouched = false;
  return t;
}
