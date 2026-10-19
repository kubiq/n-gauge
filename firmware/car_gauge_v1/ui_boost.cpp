// ============================================================================
// ui_boost.cpp  —  the boost screen: boost, the torque estimate inside it,
// and the accelerator pedal. See ui_priv.h.
// ============================================================================
#include "ui_priv.h"

extern "C" const lv_image_dsc_t icon_turbo;      // boost screen

// boost widgets
static lv_obj_t *boostArc, *boostVal, *boostIcon;
static lv_obj_t *torqueArc, *torqueVal, *pedalBar;

// BOOST, with engine torque inside it - the same two-ring layout as the
// temps screen. Torque is an ESTIMATE from absolute load (no torque value
// exists at the OBD port), so its figure says "~" and moves in 5 Nm steps.
lv_obj_t *ui_build_boost() {
  lv_obj_t *s = make_screen();
  boostArc = make_arc(s, COL_BLUE, 0, 200);  // 0..2.00 bar *100
  torqueArc = make_arc_sized(s, COL_INK, 0, TORQUE_ARC_MAX_NM, 372, 12);
  torqueVal = make_tyre_val(s, 0, 84);        // 28px, under "bar"
  // Accelerator pedal: a short bar in the open bottom quarter both arcs
  // leave (their ends sit ~148 px below centre, so 175 clears them and stays
  // inside the round panel).
  pedalBar = lv_bar_create(s);
  lv_obj_set_size(pedalBar, 120, 10);
  lv_obj_align(pedalBar, LV_ALIGN_CENTER, 0, 175);
  lv_bar_set_range(pedalBar, 0, 100);
  lv_bar_set_value(pedalBar, 0, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(pedalBar, COL_LINE, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(pedalBar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(pedalBar, COL_GOOD, LV_PART_INDICATOR);
  lv_obj_set_style_radius(pedalBar, 5, LV_PART_MAIN);
  lv_obj_set_style_radius(pedalBar, 5, LV_PART_INDICATOR);
  lv_obj_clear_flag(pedalBar, LV_OBJ_FLAG_CLICKABLE);
  // Turbo symbol beside the number, like the oil screen. The unit stays as a
  // caption: unlike a temperature, "1.23" does not say what it is in.
  lv_obj_t *boostRow = make_icon_row(s, -10, &icon_turbo, &boostIcon);
  boostVal = make_big_label(boostRow, COL_BLUE, 0);
  fix_value_width(boostVal, "8.88");                // ui_update_boost clamps < 0 to 0
  make_name(s, "bar", 40);
  return s;
}

#if FRAME_STATS
lv_obj_t *ui_boost_value() { return boostVal; }
lv_obj_t *ui_boost_arc()   { return boostArc; }
#endif

void ui_update_boost(bool have, float bar) {
  // The number shows each sample exactly, the moment it arrives. The arc
  // glides to it over BOOST_GLIDE_MS instead of jumping: samples come at
  // ~10 Hz, and a needle stepping 10 times a second reads as jerky even
  // though every position is right. The cost is that the arc trails the
  // number by up to one glide. Call every loop pass - the glide advances here.
  static bool     lastHave = false;
  static int      lastCenti = INT32_MIN;
  static float    from = 0.0f, to = 0.0f;     // bar, arc glide endpoints
  static uint32_t glideStart = 0;
  static int      arcCenti = INT32_MIN;       // what the arc shows now
  const uint32_t  now = lv_tick_get();

  // A boost gauge reads 0 below atmospheric. Negative is real manifold vacuum
  // (part throttle), but "-0.23" on a turbo gauge read as broken in the car
  // (2026-10-03) - and was clipped, the label being sized for "8.88". The
  // arc always stopped at 0; now the number does too. The CSV keeps the
  // true value.
  if (bar < 0) bar = 0;

  // Compare at display resolution (2 dp) - a float that differs in the 5th
  // decimal would defeat the guard while changing nothing on screen.
  const int centi = (int)(bar * 100.0f + (bar < 0 ? -0.5f : 0.5f));
  if (have != lastHave || centi != lastCenti) {
    // The symbol only changes with data present/absent - set it then, not on
    // every sample, since each style change redraws it.
    const bool haveChanged = have != lastHave || lastCenti == INT32_MIN;
    if (haveChanged)
      lv_obj_set_style_image_recolor(boostIcon, have ? COL_BLUE : COL_DIM, 0);
    lastHave = have; lastCenti = centi;
    if (!have) {
      // Arc to zero at once, no glide: "--" over a needle still standing at
      // the last value is a frozen reading that looks real.
      show_no_value(boostVal);
      lv_arc_set_value(boostArc, 0);
      from = to = 0.0f;
      arcCenti = 0;
      return;
    }
    if (haveChanged) show_value_style(boostVal, COL_BLUE);
    lv_label_set_text_fmt(boostVal, "%.2f", bar);
    // Start from where the arc IS, not from the last sample, so a sample that
    // lands mid-glide bends the motion instead of snapping back.
    from = arcCenti == INT32_MIN ? (bar < 0 ? 0.0f : bar) : arcCenti / 100.0f;
    to = bar < 0 ? 0.0f : bar;
    glideStart = now;
  }
  if (!have) return;

  const uint32_t t = now - glideStart;
  const float shown = t >= BOOST_GLIDE_MS ? to
                    : from + (to - from) * ((float)t / BOOST_GLIDE_MS);
  const int c = (int)(shown * 100.0f + 0.5f);
  if (c != arcCenti) {                      // 1 step = 0.01 bar, ~5 px of arc
    arcCenti = c;
    lv_arc_set_value(boostArc, c);
  }
}

void ui_update_torque(bool have, float nm) {
  // Rounded to 5 Nm for both the arc and the figure: the estimate is not
  // better than that, and finer steps would only make the number flicker.
  static bool lastHave = false;
  static int  lastNm = INT32_MIN;
  const int r = have ? 5 * (int)((nm < 0 ? 0 : nm) / 5.0f + 0.5f) : 0;
  if (have == lastHave && r == lastNm) return;
  const bool haveChanged = have != lastHave || lastNm == INT32_MIN;
  lastHave = have; lastNm = r;
  if (!have) {
    lv_label_set_text(torqueVal, "--");
    lv_obj_set_style_text_color(torqueVal, COL_DIM, 0);
    lv_arc_set_value(torqueArc, 0);
    return;
  }
  if (haveChanged) lv_obj_set_style_text_color(torqueVal, COL_INK, 0);
  lv_arc_set_value(torqueArc, r > TORQUE_ARC_MAX_NM ? TORQUE_ARC_MAX_NM : r);
  lv_label_set_text_fmt(torqueVal, "~%d Nm", r);
}

void ui_update_pedal(bool have, int pct) {
  static int last = INT32_MIN;
  const int v = !have ? 0 : pct < 0 ? 0 : pct > 100 ? 100 : pct;
  if (v == last) return;
  last = v;
  lv_bar_set_value(pedalBar, v, LV_ANIM_OFF);
}
