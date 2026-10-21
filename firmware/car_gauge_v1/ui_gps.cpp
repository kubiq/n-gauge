// ============================================================================
// ui_gps.cpp  —  the GPS screen: speed, fix state, trip distance.
// See ui_priv.h.
// ============================================================================
#include "ui_priv.h"

extern "C" const lv_image_dsc_t icon_sat;        // GPS screen

// gps widgets
static lv_obj_t *gpsSpeed, *gpsState, *gpsTrip, *gpsIcon;

lv_obj_t *ui_build_gps() {
  lv_obj_t *s = make_screen();
  // Satellite beside the speed: says "this is GPS speed" in place of the old
  // "(GPS)", and its colour is the fix state at a glance.
  lv_obj_t *gpsRow = make_icon_row(s, -10, &icon_sat, &gpsIcon);
  gpsSpeed = make_big_label(gpsRow, COL_GOOD, 0);
  fix_value_width(gpsSpeed, "888");
  make_name(s, "km/h", 40);
  gpsState = make_name(s, "", 80);
  // Below the fix state rather than beside the speed: at 466 px the speed is
  // already the full chord width, and crowding a second number next to it
  // would shrink both.
  gpsTrip  = make_tyre_val(s, 0, 120);
  return s;
}

void ui_update_gps(bool linkUp, bool haveFix, int sats, float speedKmh,
                   float tripKm) {
  // The trip label has its OWN guard. It used to share the one below, so every
  // speed or satellite change re-set an unchanged trip label (and every trip
  // tick redrew speed and state) - the needless invalidation the guard exists
  // to prevent.
  //
  // Compare the DISPLAYED value, in tenths, not the float: at 25 Hz the raw
  // distance changes on every accepted fix, so comparing floats would redraw
  // continuously and blink the label - the same trap as the oil number.
  //
  // Always shown, even with no fix: it is the distance already travelled, and
  // losing signal does not un-travel it.
  static int lastTrip = INT32_MIN;
  const int trip = (int)(tripKm * 10.0f + 0.5f);
  if (trip != lastTrip) {
    if (lastTrip == INT32_MIN) lv_obj_set_style_text_color(gpsTrip, COL_INK, 0);
    lastTrip = trip;
    lv_label_set_text_fmt(gpsTrip, "%d.%d km", trip / 10, trip % 10);
  }

  static bool lastLink = false, lastFix = false;
  static int  lastSats = INT32_MIN, lastKmh = INT32_MIN;
  const int kmh  = (int)(speedKmh + 0.5f);       // what actually gets shown
  if (linkUp == lastLink && haveFix == lastFix &&
      sats == lastSats && kmh == lastKmh) return;
  // The satellite shows the state: grey no module, blue searching, green fix.
  // Only when the state changes - not with every speed or satellite count.
  if (linkUp != lastLink || haveFix != lastFix || lastSats == INT32_MIN)
    lv_obj_set_style_image_recolor(gpsIcon,
        !linkUp ? COL_DIM : haveFix ? COL_GOOD : COL_BLUE, 0);
  lastLink = linkUp; lastFix = haveFix; lastSats = sats; lastKmh = kmh;

  if (!linkUp) {                       // no NMEA at all: wiring or baud
    show_no_value(gpsSpeed);
    lv_label_set_text(gpsState, "no module");
    set_text_color(gpsState, COL_BAD);
    return;
  }
  if (!haveFix) {                      // talking, but no sky view yet
    show_no_value(gpsSpeed);
    lv_label_set_text_fmt(gpsState, "searching  %d sats", sats);
    set_text_color(gpsState, COL_BLUE);
    return;
  }
  lv_label_set_text_fmt(gpsSpeed, "%d", (int)(speedKmh + 0.5f));
  show_value_style(gpsSpeed, COL_GOOD);
  lv_label_set_text_fmt(gpsState, "fix  %d sats", sats);
  set_text_color(gpsState, COL_GOOD);
}
