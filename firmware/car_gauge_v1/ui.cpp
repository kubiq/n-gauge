// ============================================================================
// ui.cpp  —  LVGL UI.  466x466 round AMOLED. Builds every screen and switches
//   between them; the screens themselves are ui_<screen>.cpp - see ui_priv.h.
//   Uses LVGL ~9.x API. If Waveshare's example ships LVGL 8.x, the only
//   likely tweaks are lv_scr_load -> lv_screen_load and a couple of enum names.
// ============================================================================
#include "ui_priv.h"
#include "settings.h"   // the cycle order, settings.views
#include "power.h"      // a screen change is a sign of life; hold off sleep
#include <string.h>     // strchr/strlen on the cycle order

static GaugeView s_view = VIEW_OIL;
static lv_obj_t *scr[VIEW_COUNT];

// --- build ------------------------------------------------------------------
void ui_init() {
  scr[VIEW_OIL]    = ui_build_temps();
  scr[VIEW_BOOST]  = ui_build_boost();
  scr[VIEW_GMETER] = ui_build_gmeter();
  scr[VIEW_TPMS]   = ui_build_tyres();
  scr[VIEW_GPS]    = ui_build_gps();
  scr[VIEW_TRACK]  = ui_build_track();
  scr[VIEW_WIFI]   = ui_build_wifi();

  // Readouts must not be interactive, but LVGL makes arcs AND plain objects
  // clickable by default (lv_arc.c: lv_obj_add_flag(CLICKABLE), lv_obj.c:
  // flags = CLICKABLE). A clickable arc also reacts to a drag by changing its
  // value, so a swipe starting on a gauge would move the needle. Strip the flag
  // from every child, instead of relying on each widget remembering to.
  //
  // Buttons are the one exception - being clicked is their whole job. Skipped
  // by TYPE, so a future button on any screen keeps working without having to
  // remember this loop exists.
  for (int v = 0; v < VIEW_COUNT; v++) {
    if (!scr[v]) continue;          // disabled view (e.g. g-meter when off)
    uint32_t n = lv_obj_get_child_count(scr[v]);
    for (uint32_t i = 0; i < n; i++) {
      lv_obj_t *c = lv_obj_get_child(scr[v], i);
      if (lv_obj_check_type(c, &lv_button_class)) continue;
      lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);
    }
  }

  ui_build_notice();

  // A swipe that STARTS on the notification text must still change screen.
  // Its gesture bubbles label -> rim -> lv_layer_top, and stops there because
  // the top layer has no parent - so it would reach no screen at all. The top
  // layer gets the same handler the screens have.
  lv_obj_add_event_cb(lv_layer_top(), screen_gesture_cb, LV_EVENT_GESTURE, NULL);

  // No AP banner. It existed to announce a briefly-toggled AP; the AP is now
  // up the whole time the board is awake, so a permanent badge on every screen
  // is just noise - and VIEW_WIFI carries the name, passphrase and URL.

  lv_scr_load(scr[VIEW_OIL]);
}

#if FRAME_STATS
lv_obj_t *ui_screen(GaugeView v) { return scr[v]; }
#endif

// --- switching --------------------------------------------------------------
// The cycle order is settings.views - set from the app, default
// VIEW_ORDER_DEFAULT - and read on every step, so a change applies at the
// next tap. A view left out of it is never cycled to.

void ui_set_view(GaugeView v) {
  if (v < 0 || v >= VIEW_COUNT || !scr[v]) return;   // never load a disabled view
  s_view = v;
  lv_scr_load(scr[v]);
}

void ui_step_view(int delta) {
  // The one chokepoint for "a person asked for a different screen" - swipes
  // (screen_gesture_cb) come through here, so the sleep timer is reset here.
  // Deliberately NOT in ui_set_view(), which also runs for the boot animation
  // and for restoring the saved screen - neither of which is a person.
  power_note_activity();
  const char *order = settings.views;
  const int n = (int)strlen(order);          // >= 1: it always holds VIEW_WIFI
  const char *at = strchr(order, '0' + s_view);
  // From a view not in the order (hidden since it was shown), start at
  // either end. + n keeps it non-negative for delta = -1, since % on a
  // negative left operand is negative in C.
  const int i = at ? (int)(at - order + n + delta) % n : (delta > 0 ? 0 : n - 1);
  ui_set_view((GaugeView)(order[i] - '0'));
}

GaugeView ui_current_view()  { return s_view; }

bool ui_view_shown(GaugeView v) { return strchr(settings.views, '0' + v) != nullptr; }

GaugeView ui_home_view() {
  for (const char *p = settings.views; *p; p++)
    if (*p - '0' != VIEW_WIFI) return (GaugeView)(*p - '0');
  return VIEW_OIL;                   // everything hidden but WiFi
}
