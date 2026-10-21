// ============================================================================
// ui_temps.cpp  —  the oil and coolant screen. See ui_priv.h.
// ============================================================================
#include "ui_priv.h"
#include <string.h>     // strcmp: the state line, set only on a change

// Oil-temperature and coolant-temperature warning-light symbols for the oil
// screen: A8 masks (icon_oil.c, icon_coolant.c), coloured on the device.
extern "C" const lv_image_dsc_t icon_oil;
extern "C" const lv_image_dsc_t icon_coolant;

// oil widgets
static lv_obj_t *oilArc, *oilVal, *oilState, *oilIcon;
static lv_obj_t *coolIcon;
// coolant widgets
static lv_obj_t *coolArc, *coolVal;

// Whether the car is answering at all - says WHY a value is missing on the
// oil screen's state line. Set from loop() via ui_set_car_talking().
static bool carTalking = false;

// TEMPS: oil and coolant on ONE screen, as concentric arcs. They are read
// together, they fail together, and flipping between two nearly identical
// dials while driving was more work than it was worth. Oil is the outer
// arc and the big number because it is the one this gauge exists for;
// coolant is the inner arc and a smaller figure under it.
lv_obj_t *ui_build_temps() {
  lv_obj_t *s = make_screen();
  oilArc   = make_arc_sized(s, COL_OIL,  0, 150, RING_OUTER_PX, RING_OUTER_W);
  coolArc  = make_arc_sized(s, COL_BLUE, 0, 130, RING_INNER_PX, RING_INNER_W);
  // Each reading is its dashboard symbol beside the number, in place of the
  // old "OIL / COOLANT" caption - the same symbols as the car's own warning
  // lights, so they need no reading. Coloured by state in the updaters.
  lv_obj_t *oilRow  = make_icon_row(s, -34, &icon_oil,     &oilIcon);
  lv_obj_t *coolRow = make_icon_row(s,  16, &icon_coolant, &coolIcon);
  oilVal   = make_big_label(oilRow, COL_OIL, 0);
  coolVal  = make_tyre_val(coolRow, 0, 0);          // 28px label, reused
  lv_obj_set_style_text_color(coolVal, COL_BLUE, 0);
  fix_value_width(oilVal,  "888\xC2\xB0");          // widest: three digits + degree
  // One column (2026-10-09): each row is centred on its own, and the coolant
  // number is the smaller font, so its row was narrower and the two symbols
  // sat at different x. The same slot width for both makes the rows equal,
  // so the symbols line up and the numbers start at the same x.
  lv_obj_set_width(coolVal, lv_obj_get_style_width(oilVal, 0));
  lv_obj_set_style_text_align(coolVal, LV_TEXT_ALIGN_LEFT, 0);
  lv_label_set_long_mode(coolVal, LV_LABEL_LONG_CLIP);
  // And the same size: the two symbols are different artwork, 58x46 (oil)
  // and 43x40 (coolant). Each goes in a 50x40 box, the oil one scaled to the
  // coolant one's height, both centred in it.
  lv_image_set_scale(oilIcon, 256 * 40 / 46);       // -> ~50x40
  lv_obj_t *const tempIcons[] = { oilIcon, coolIcon };
  for (lv_obj_t *ic : tempIcons) {
    lv_obj_set_size(ic, 50, 40);
    lv_image_set_inner_align(ic, LV_IMAGE_ALIGN_CENTER);
  }
  // One state line for both: coolant has none of its own - an over-temp
  // coolant shows as a red inner arc, and this line carries the wording.
  oilState = make_name(s, "", 84);
  return s;
}

void ui_set_car_talking(bool talking) { carTalking = talking; }

void ui_update_oil(bool have, int tempC, bool cold, bool overOil, bool overCool) {
  static bool lastHave = false, lastCold = false, lastOverOil = false, lastOverCool = false;
  static bool lastTalking = false;
  static int  lastTemp = INT32_MIN;
  if (have == lastHave && tempC == lastTemp && cold == lastCold &&
      overOil == lastOverOil && overCool == lastOverCool && carTalking == lastTalking)
    return;
  const bool haveChanged = have != lastHave || lastTemp == INT32_MIN;
  lastHave = have; lastTemp = tempC; lastCold = cold;
  lastOverOil = overOil; lastOverCool = overCool;
  lastTalking = carTalking;

  if (!have) {
    show_no_value(oilVal);
    // Zero and de-colour the arc too. Leaving it where it froze meant "--"
    // above a full red arc after a dropout at 125 C, which is exactly the
    // "a frozen reading must not look real" rule OBD_STALE_MS exists for.
    lv_arc_set_value(oilArc, 0);
    set_arc_color(oilArc, COL_OIL);
    set_recolor(oilIcon, COL_DIM);
    // The state line is SHARED with coolant, so blanking it unconditionally threw away a coolant over-temp warning
    // whenever the oil DID stopped answering - and coolant is the failure that
    // wrecks an engine fastest. Keep the warning if coolant is still over.
    if (overCool) {
      lv_label_set_text(oilState, "COOLANT TOO HOT");
      set_text_color(oilState, COL_BAD);
    } else {
      // Why there is no number, instead of an empty line: the car silent
      // (just woken, ignition off) or talking but this value gone stale.
      lv_label_set_text(oilState, carTalking ? "no oil reading" : "waiting for car");
      set_text_color(oilState, COL_DIM);
    }
    return;
  }
  if (haveChanged) show_value_style(oilVal, COL_OIL);
  lv_arc_set_value(oilArc, tempC);
  lv_label_set_text_fmt(oilVal, "%d\xC2\xB0", tempC);
  // The symbol takes the arc's colour: blue cold, oil orange ready, red hot -
  // the OIL's state only. It used to take coolant's too, so a hot coolant
  // turned the oil arc and symbol red over a perfectly good oil reading.
  const lv_color_t stateC = overOil ? COL_BAD : cold ? COL_BLUE : COL_OIL;
  set_arc_color(oilArc, stateC);
  set_recolor(oilIcon, stateC);
  // The shared state line names which one is hot. Its text only on a change
  // too: lv_label_set_text() redraws the label even with the same text.
  const char *state; lv_color_t stateTextC;
  if (overOil && overCool) { state = "OIL + COOLANT TOO HOT"; stateTextC = COL_BAD; }
  else if (overOil)        { state = "OIL TOO HOT";           stateTextC = COL_BAD; }
  else if (overCool)       { state = "COOLANT TOO HOT";       stateTextC = COL_BAD; }
  else if (cold)           { state = "COLD - take it easy";   stateTextC = COL_BLUE; }
  else                     { state = "READY";                 stateTextC = COL_GOOD; }
  if (strcmp(lv_label_get_text(oilState), state) != 0) lv_label_set_text(oilState, state);
  set_text_color(oilState, stateTextC);
}

void ui_update_coolant(bool have, int tempC, bool over) {
  static bool lastHave = false, lastOver = false;
  static int  lastTemp = INT32_MIN;
  if (have == lastHave && tempC == lastTemp && over == lastOver) return;
  lastHave = have; lastTemp = tempC; lastOver = over;

  if (!have) {
    // Arc and colour too, for the same reason as in ui_update_oil.
    show_no_value(coolVal);
    lv_arc_set_value(coolArc, 0);
    set_arc_color(coolArc, COL_BLUE);
    set_recolor(coolIcon, COL_DIM);
    return;
  }
  lv_arc_set_value(coolArc, tempC);
  lv_label_set_text_fmt(coolVal, "%d\xC2\xB0", tempC);
  // Colour only. The shared state line is owned by ui_update_oil, which is
  // called first each loop and already says TOO HOT when either is over.
  const lv_color_t c = over ? COL_BAD : COL_BLUE;
  set_arc_color(coolArc, c);
  show_value_style(coolVal, c);                 // its colour, and the alignment back
  set_recolor(coolIcon, c);
}
