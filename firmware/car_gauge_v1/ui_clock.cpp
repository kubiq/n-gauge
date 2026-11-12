// ============================================================================
// ui_clock.cpp  —  an analog clock (asked 2026-11-13): the hour numbers and
// minute ticks round the panel, three hands, the date under the centre. Local
// time, as the gauge's TZ setting makes it. Without a valid clock (a cold
// start before the first GPS fix) the hands are hidden and it says so.
//
// One redraw a second, for the second hand: a hand is a line object, and
// moving it invalidates only the box it sweeps, not the panel. Optional, as
// every screen: shown from the app's screen order. See ui_priv.h.
// ============================================================================
#include "ui_priv.h"
#include <math.h>       // cosf/sinf for the hands
#include <string.h>     // strncmp/strlcpy for the date

static const int CX = LCD_PX / 2, CY = LCD_PX / 2;   // the panel's centre

struct Hand {
  lv_obj_t *line;
  lv_point_precise_t pts[2];
  int length, tail;
};
static Hand hourH, minH, secH;
static lv_obj_t *cap, *dateL, *noClock;

static void makeHand(lv_obj_t *s, Hand &h, int length, int tail, int width, lv_color_t c) {
  h.length = length;
  h.tail = tail;
  h.line = lv_line_create(s);
  lv_obj_set_pos(h.line, 0, 0);
  lv_obj_set_style_line_width(h.line, width, 0);
  lv_obj_set_style_line_color(h.line, c, 0);
  lv_obj_set_style_line_rounded(h.line, true, 0);
  h.pts[0] = {(lv_value_precise_t)CX, (lv_value_precise_t)CY};
  h.pts[1] = {(lv_value_precise_t)CX, (lv_value_precise_t)(CY - length)};
  lv_line_set_points_mutable(h.line, h.pts, 2);
}

// `turn`: 0..1 of a full turn from twelve, clockwise.
static void setHand(Hand &h, float turn) {
  const float a = turn * 2.0f * (float)M_PI;
  const float dx = sinf(a), dy = -cosf(a);
  h.pts[0] = {(lv_value_precise_t)(CX - dx * h.tail), (lv_value_precise_t)(CY - dy * h.tail)};
  h.pts[1] = {(lv_value_precise_t)(CX + dx * h.length), (lv_value_precise_t)(CY + dy * h.length)};
  lv_line_set_points_mutable(h.line, h.pts, 2);   // new size, old and new box invalidated
}

lv_obj_t *ui_build_clock() {
  lv_obj_t *s = make_screen();

  // The face: 60 minute ticks round the edge, every fifth longer with its
  // hour - LVGL's round scale draws both, once.
  static const char *hours[] = {"12", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", NULL};
  lv_obj_t *face = lv_scale_create(s);
  lv_obj_set_size(face, RING_EDGE_PX, RING_EDGE_PX);
  lv_obj_center(face);
  lv_scale_set_mode(face, LV_SCALE_MODE_ROUND_INNER);
  lv_scale_set_range(face, 0, 60);
  lv_scale_set_total_tick_count(face, 61);
  lv_scale_set_major_tick_every(face, 5);
  lv_scale_set_angle_range(face, 360);
  lv_scale_set_rotation(face, 270);              // 0 at the top
  lv_scale_set_label_show(face, true);
  lv_scale_set_text_src(face, hours);
  lv_obj_set_style_bg_opa(face, LV_OPA_TRANSP, 0);
  lv_obj_set_style_arc_opa(face, LV_OPA_TRANSP, LV_PART_MAIN);     // no ring line
  lv_obj_set_style_length(face, 18, LV_PART_INDICATOR);            // hour ticks
  lv_obj_set_style_line_width(face, 4, LV_PART_INDICATOR);
  lv_obj_set_style_line_color(face, COL_INK, LV_PART_INDICATOR);
  lv_obj_set_style_text_font(face, &lv_font_montserrat_28, LV_PART_INDICATOR);
  lv_obj_set_style_text_color(face, COL_INK, LV_PART_INDICATOR);
  lv_obj_set_style_length(face, 8, LV_PART_ITEMS);                 // minute ticks
  lv_obj_set_style_line_width(face, 2, LV_PART_ITEMS);
  lv_obj_set_style_line_color(face, COL_DIM, LV_PART_ITEMS);

  dateL = make_name(s, "", 70);
  lv_obj_set_style_text_color(dateL, COL_DIM, 0);
  noClock = make_name(s, "no clock yet - waiting for GPS", 70);
  lv_obj_set_style_text_color(noClock, COL_DIM, 0);

  makeHand(s, hourH, 105, 18, 10, COL_INK);
  makeHand(s, minH, 160, 22, 6, COL_INK);
  makeHand(s, secH, 175, 35, 2, COL_OIL);

  cap = lv_obj_create(s);
  lv_obj_set_size(cap, 16, 16);
  lv_obj_center(cap);
  lv_obj_set_style_radius(cap, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(cap, COL_OIL, 0);
  lv_obj_set_style_border_width(cap, 0, 0);
  lv_obj_clear_flag(cap, LV_OBJ_FLAG_SCROLLABLE);
  return s;
}

void ui_update_clock(bool valid, int hour, int minute, int second, const char *date) {
  static int lastValid = -1;
  if ((int)valid != lastValid) {
    lastValid = valid;
    lv_obj_t *const shown[] = {hourH.line, minH.line, secH.line, dateL};
    for (lv_obj_t *o : shown) {
      if (valid) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
      else       lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
    if (valid) lv_obj_add_flag(noClock, LV_OBJ_FLAG_HIDDEN);
    else       lv_obj_clear_flag(noClock, LV_OBJ_FLAG_HIDDEN);
  }
  if (!valid) return;
  setHand(secH, second / 60.0f);
  setHand(minH, (minute + second / 60.0f) / 60.0f);
  setHand(hourH, ((hour % 12) + minute / 60.0f) / 12.0f);
  static char lastDate[24] = "";
  if (strncmp(lastDate, date, sizeof(lastDate) - 1)) {
    strlcpy(lastDate, date, sizeof(lastDate));
    lv_label_set_text(dateL, lastDate);
  }
}
