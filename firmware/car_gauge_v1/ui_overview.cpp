// ============================================================================
// ui_overview.cpp  —  the overview: everything the car tells, on one screen
// (asked 2026-11-13). Oil and coolant as the oil screen has them, the two
// rings and their colours; boost large in the centre; the fuel - this
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
static lv_obj_t *boostVal, *fuelAvg, *fuelSmall;
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

  boostVal = make_tyre_val(s, 0, -48);
  lv_obj_set_style_text_font(boostVal, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(boostVal, COL_BLUE, 0);
  lv_obj_t *boostCap = make_name(s, "boost bar", -12);
  lv_obj_set_style_text_color(boostCap, COL_DIM, 0);

  fuelAvg = make_tyre_val(s, 0, 24);
  fuelSmall = make_name(s, "", 52);
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

  // Boost: as the boost screen, not below 0.
  if (o.haveBoost) snprintf(t, sizeof(t), "%.2f", o.boostBar < 0 ? 0.0f : o.boostBar);
  put(boostVal, o.haveBoost ? t : "--", o.haveBoost ? COL_BLUE : COL_DIM);

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
