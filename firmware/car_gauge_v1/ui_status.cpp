// ============================================================================
// ui_status.cpp  —  the status screen: the log upload to the home server and
// the firmware update it may bring (upload.h), in words - so it can be seen
// that something is happening while the gauge's WiFi is away for it (asked
// 2026-11-09). Hidden by default (not in VIEW_ORDER_DEFAULT): shown from the
// app's screen order. See ui_priv.h.
// ============================================================================
#include "ui_priv.h"
#include <string.h>     // strncmp/strncpy in the update guard

static lv_obj_t *stHead, *stDetail, *stLast, *stFw, *stCar;

// A wrapped, centred line: the panel is round, so the lower ones are narrower.
static lv_obj_t *make_line(lv_obj_t *s, int dy, int width, lv_color_t c) {
  lv_obj_t *l = make_name(s, "", dy);
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(l, width);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(l, c, 0);
  lv_obj_align(l, LV_ALIGN_CENTER, 0, dy);
  return l;
}

lv_obj_t *ui_build_status() {
  lv_obj_t *s = make_screen();
  lv_obj_t *title = make_name(s, "HOME SERVER", -150);
  lv_obj_set_style_text_color(title, COL_DIM, 0);
  // What it is doing, big; then the step or what it waits for.
  stHead = make_tyre_val(s, 0, -85);
  lv_label_set_text(stHead, "");
  stDetail = make_line(s, -35, 340, COL_INK);
  // The last upload, then a firmware waiting to install.
  stLast = make_line(s, 30, 330, COL_DIM);
  stFw = make_line(s, 95, 380, COL_OIL);
  // The car's link, low: "not connected to CAN bus" lives here since
  // 2026-11-12, not as a rim over every screen.
  stCar = make_line(s, 150, 300, COL_DIM);
  return s;
}

void ui_update_status_car(uint8_t tone, const char *text) {
  static int lastTone = -1;
  static char t[40] = "\x01";
  if (tone != lastTone) {
    lastTone = tone;
    lv_obj_set_style_text_color(stCar, tone == 2 ? COL_BAD : tone == 1 ? COL_GOOD : COL_DIM, 0);
  }
  if (strncmp(t, text, sizeof(t) - 1)) {
    strncpy(t, text, sizeof(t) - 1);
    t[sizeof(t) - 1] = '\0';
    lv_label_set_text(stCar, t);
  }
}

// Each text only when it changed: the labels redraw on every set.
static void setIf(lv_obj_t *l, char *was, size_t n, const char *now) {
  if (!strncmp(was, now ? now : "", n - 1)) return;
  strncpy(was, now ? now : "", n - 1);
  was[n - 1] = '\0';
  lv_label_set_text(l, was);
}

void ui_update_status(uint8_t tone, const char *head, const char *detail,
                      const char *last, const char *fw) {
  static int lastTone = -1;
  static char h[24] = "\x01", d[64] = "\x01", l[64] = "\x01", f[64] = "\x01";
  if (tone != lastTone) {
    lastTone = tone;
    lv_obj_set_style_text_color(stHead, tone == 1 ? COL_BLUE : tone == 2 ? COL_GOOD : tone == 3 ? COL_BAD : COL_INK, 0);
  }
  setIf(stHead, h, sizeof(h), head);
  setIf(stDetail, d, sizeof(d), detail);
  setIf(stLast, l, sizeof(l), last);
  setIf(stFw, f, sizeof(f), fw);
}
