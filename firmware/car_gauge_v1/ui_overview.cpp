// ============================================================================
// ui_overview.cpp  —  the overview: everything the car tells, on one screen
// (asked 2026-11-13). Oil and coolant as the oil screen has them, the two
// rings and their colours; the power large in the centre, boost under it
// while there is some (2026-11-22; rpm 2026-11-21, the gear 2026-11-17,
// boost alone before); the fuel - this
// drive's average large, the instant value and the tank small; the tyres
// round a small car at the bottom. First in the default order; the other
// screens stay, for the detail. See ui_priv.h.
// ============================================================================
#include "ui_priv.h"
#include <stdio.h>      // snprintf
#include <string.h>     // strncmp: each text set only on a change

extern "C" const lv_image_dsc_t icon_oil;
extern "C" const lv_image_dsc_t icon_coolant;
extern "C" const lv_image_dsc_t car_top;

static lv_obj_t *oilArc, *coolArc, *oilIcon, *coolIcon, *oilVal, *coolVal;
static lv_obj_t *powerVal, *boostSmall, *fuelAvg, *fuelSmall;
static lv_obj_t *tyreVal[WHEEL_COUNT];

static lv_obj_t *smallIconRow(lv_obj_t *s, int dx, int dy, const lv_image_dsc_t *icon, int iconH,
                              lv_obj_t **iconOut, lv_obj_t **valOut) {
  lv_obj_t *row = make_icon_row(s, dy, icon, iconOut);
  lv_obj_align(row, LV_ALIGN_CENTER, dx, dy);
  lv_image_set_scale(*iconOut, 256 * 26 / iconH);   // ~26 px high beside a 28 px number
  lv_obj_set_size(*iconOut, 34, 26);
  lv_image_set_inner_align(*iconOut, LV_IMAGE_ALIGN_CENTER);
  *valOut = make_tyre_val(row, 0, 0);
  return row;
}

lv_obj_t *ui_build_overview() {
  lv_obj_t *s = make_screen();
  // The oil screen's rings: oil outer, coolant inner, the same ranges.
  oilArc  = make_arc_sized(s, COL_OIL,  0, 150, RING_OUTER_PX, RING_OUTER_W);
  coolArc = make_arc_sized(s, COL_BLUE, 0, 130, RING_INNER_PX, RING_INNER_W);
  smallIconRow(s, -62, -112, &icon_oil, 46, &oilIcon, &oilVal);
  smallIconRow(s, 62, -112, &icon_coolant, 40, &coolIcon, &coolVal);

  // The power, large (asked 2026-11-22; rpm 2026-11-21 flickered and the
  // car has a rev counter, the gear 2026-11-17, and boost alone before that
  // read 0.00 all through town); its unit and boost small under it.
  powerVal = make_tyre_val(s, 0, -52);
  lv_obj_set_style_text_font(powerVal, &font_big_72, 0);
  lv_obj_set_style_text_color(powerVal, COL_INK, 0);
  boostSmall = make_name(s, "kW", -8);
  lv_obj_set_style_text_color(boostSmall, COL_DIM, 0);

  // The average a size up from the tyres' 28 (asked 2026-11-22).
  fuelAvg = make_tyre_val(s, 0, 28);
  lv_obj_set_style_text_font(fuelAvg, &lv_font_montserrat_36, 0);
  fuelSmall = make_name(s, "", 58);
  lv_obj_set_style_text_color(fuelSmall, COL_DIM, 0);

  lv_obj_t *car = lv_image_create(s);
  lv_image_set_src(car, &car_top);
  lv_image_set_scale(car, 64);                       // 200x278 -> 50x70
  lv_obj_set_size(car, 50, 70);
  lv_image_set_inner_align(car, LV_IMAGE_ALIGN_CENTER);
  lv_obj_align(car, LV_ALIGN_CENTER, 0, 112);
  static const int ty[WHEEL_COUNT] = {92, 92, 132, 132};
  for (int i = 0; i < WHEEL_COUNT; i++)
    tyreVal[i] = make_tyre_val(s, (i % 2) ? 60 : -60, ty[i]);
  return s;
}

// A label's text and colour, each only on a change: setting either redraws it.
static void put(lv_obj_t *l, const char *txt, lv_color_t c) {
  if (strncmp(lv_label_get_text(l), txt, 31)) lv_label_set_text(l, txt);
  set_text_color(l, c);
}

void ui_update_overview(const Overview &o) {
  char t[32];
  // Oil and coolant: number, ring and symbol in the oil screen's colours.
  const lv_color_t oilC = o.oilState == 2 ? COL_BAD : o.oilState == 0 ? COL_BLUE : COL_OIL;
  if (o.haveOil) snprintf(t, sizeof(t), "%d\xC2\xB0", o.oilC);
  put(oilVal, o.haveOil ? t : "--", o.haveOil ? oilC : COL_DIM);
  lv_arc_set_value(oilArc, o.haveOil ? o.oilC : 0);
  set_arc_color(oilArc, o.haveOil ? oilC : COL_OIL);
  set_recolor(oilIcon, o.haveOil ? oilC : COL_DIM);
  const lv_color_t coolC = o.coolHot ? COL_BAD : COL_BLUE;
  if (o.haveCool) snprintf(t, sizeof(t), "%d\xC2\xB0", o.coolC);
  put(coolVal, o.haveCool ? t : "--", o.haveCool ? coolC : COL_DIM);
  lv_arc_set_value(coolArc, o.haveCool ? o.coolC : 0);
  set_arc_color(coolArc, coolC);
  set_recolor(coolIcon, o.haveCool ? coolC : COL_DIM);

  // The power: "-" with the engine off (font_big_72 has the digits and "-"
  // only, so its unit goes in the small line).
  if (o.havePower) snprintf(t, sizeof(t), "%d", o.powerKw);
  put(powerVal, o.havePower ? t : "-", o.havePower ? COL_INK : COL_DIM);
  // Under it "kW", and boost after it while the turbo is giving some.
  const bool boosting = o.haveBoost && o.boostBar >= OVERVIEW_BOOST_FROM;
  if (boosting) snprintf(t, sizeof(t), "kW, boost %.2f bar", o.boostBar);
  put(boostSmall, boosting ? t : "kW", boosting ? COL_BLUE : COL_DIM);

  // Fuel: the drive's average large; the instant value and the tank small.
  if (o.haveAvg) snprintf(t, sizeof(t), "%.1f l/100", o.avgL100);
  put(fuelAvg, o.haveAvg ? t : "-- l/100", o.haveAvg ? COL_INK : COL_DIM);
  char now[16] = "--", tank[12] = "--";
  if (o.haveNow) snprintf(now, sizeof(now), o.nowPerHour ? "%.1f l/h" : "%.1f", o.now);
  if (o.haveTank) snprintf(tank, sizeof(tank), "%d %%", o.tankPct);
  // Spaces between, not a middle dot: the 14 px font has no "·" (a box).
  snprintf(t, sizeof(t), "now %s     tank %s", now, tank);
  put(fuelSmall, t, COL_DIM);

  // Tyres: pressure, red outside its axle's limits.
  for (int i = 0; i < WHEEL_COUNT; i++) {
    if (o.haveTyre[i]) snprintf(t, sizeof(t), "%.1f", o.tyreBar[i]);
    put(tyreVal[i], o.haveTyre[i] ? t : "--", !o.haveTyre[i] ? COL_DIM : o.tyreBad[i] ? COL_BAD : COL_INK);
  }
}
