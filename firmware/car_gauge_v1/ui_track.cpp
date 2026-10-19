// ============================================================================
// ui_track.cpp  —  the track screen: the lap timer's face and its START and
// STOP buttons. See ui_priv.h; the timing itself is laps.cpp.
// ============================================================================
#include "ui_priv.h"
#include "power.h"      // a tap is a sign of life; hold off sleep
#include "laps.h"       // the track screen's buttons drive the lap timer
#include "calc.h"       // calc_lap_left_m: how far the line is
#include <math.h>       // lroundf, floorf
#include <stdio.h>      // snprintf
#include <string.h>     // strcmp/strncpy in the status guard

extern "C" const lv_image_dsc_t icon_flag;       // track screen

// track (lap timer) widgets
static lv_obj_t *trkCurrent, *trkLast, *trkBest, *trkStatus, *trkFlag, *trkLeft, *trkDelta;
// SET and STOP share one slot: SET while no session is running, STOP once one
// is. Only one is ever visible, so neither sits on screen doing nothing.
static lv_obj_t *trkSetBtn, *trkStopBtn;

// Transient message on the track screen ("tap again to stop", "no GPS fix"),
// shown in place of the normal status until it expires.
static const char *trkMsg = NULL;
static uint32_t    trkMsgUntil = 0;
// Which button is waiting for its confirming second tap, and until when.
enum { TRK_CONFIRM_NONE, TRK_CONFIRM_SET, TRK_CONFIRM_STOP };
static int      trkConfirm = TRK_CONFIRM_NONE;
static uint32_t trkConfirmUntil = 0;

static void trkFlash(const char *m) {
  trkMsg      = m;
  // lv_tick_get(), not millis(): it is millis() on this board (bsp.cpp sets
  // the tick callback), and it keeps Arduino out of the UI layer.
  trkMsgUntil = lv_tick_get() + TRACK_CONFIRM_MS;
}

// START and STOP on the track screen. The button's identity arrives as the
// event's user data (TRK_CONFIRM_SET / TRK_CONFIRM_STOP). Only one of the two
// is visible at a time - see ui_update_track().
//
// START is only shown with no session running, so it acts at once: there is
// nothing to lose. It is tapped parked - the pit, the paddock - and the line
// is then found while driving (laps.h). STOP needs a second tap within
// TRACK_CONFIRM_MS, because the screen is in the driver's reach and one
// brushed tap mid-session would otherwise wipe every lap.
static void track_btn_cb(lv_event_t *e) {
  if (touch_was_swipe()) return;         // a swipe that started on the button
  power_note_activity();

  const int      which = (int)(intptr_t)lv_event_get_user_data(e);
  const uint32_t now   = lv_tick_get();

  if (which == TRK_CONFIRM_SET) {
    trkMsg = NULL;
    laps_start();
    return;
  }

  if (!(trkConfirm == TRK_CONFIRM_STOP && (int32_t)(now - trkConfirmUntil) < 0)) {
    trkConfirm      = TRK_CONFIRM_STOP;
    trkConfirmUntil = now + TRACK_CONFIRM_MS;
    trkFlash("tap STOP again to end the session");
    return;
  }
  trkConfirm = TRK_CONFIRM_NONE;
  trkMsg     = NULL;
  laps_stop();
}

// A pill-shaped outline button: `c` is its colour, `which` its identity for
// track_btn_cb().
static lv_obj_t* make_button(lv_obj_t *parent, const char *txt, lv_color_t c,
                             int dx, int dy, int which) {
  lv_obj_t *b = lv_button_create(parent);
  lv_obj_set_size(b, 130, 52);
  lv_obj_align(b, LV_ALIGN_CENTER, dx, dy);
  lv_obj_set_style_radius(b, 26, 0);
  lv_obj_set_style_bg_color(b, COL_BG, 0);
  lv_obj_set_style_bg_color(b, COL_LINE, LV_STATE_PRESSED);   // press feedback
  lv_obj_set_style_border_color(b, c, 0);
  lv_obj_set_style_border_width(b, 2, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  // Not scrollable, so a swipe that starts on the button is recognised as a
  // gesture (and changes screen) instead of trying to scroll the button.
  lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(b, track_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)which);

  lv_obj_t *l = lv_label_create(b);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_color(l, c, 0);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
  // The label must not take the touch itself, or a tap on the text would land
  // on the label and the button would never see a click.
  lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_center(l);
  return b;
}

// TRACK: lap timer. Current lap big in the middle, last and best under it,
// a status line saying what to do next, and the two buttons.
//
// The buttons sit at +118, not lower: the notification rim's text line
// occupies roughly +163..+179 on this round panel, and below +150 the chord
// also narrows too fast for two 130 px buttons side by side.
lv_obj_t *ui_build_track() {
  lv_obj_t *s = make_screen();
  // How far the line is, against the best lap's length: an arc round the
  // edge filling up towards it - full at the line. First, so it
  // is under everything - and not clickable (make_arc_sized), so the
  // buttons above it still take every tap.
  trkLeft = make_arc_sized(s, COL_BLUE, 0, TRACK_ARC_STEPS, 440, 12);
  lv_obj_add_flag(trkLeft, LV_OBJ_FLAG_HIDDEN);
  // Chequered flag in place of the "LAP TIMER" caption (Material Symbols
  // "sports_score", Apache 2.0 - see firmware/assets/). Coloured by session
  // state in ui_update_track().
  trkFlag = lv_image_create(s);
  lv_image_set_src(trkFlag, &icon_flag);
  lv_obj_set_style_image_recolor_opa(trkFlag, LV_OPA_COVER, 0);
  lv_obj_set_style_image_recolor(trkFlag, COL_DIM, 0);
  lv_obj_align(trkFlag, LV_ALIGN_CENTER, 0, -150);
  lv_obj_clear_flag(trkFlag, LV_OBJ_FLAG_CLICKABLE);
  // The live delta to the best lap, in the flag's place while there is one
  // (LapInfo.haveDelta): "-1.2" green, ahead; "+0.8" red, behind.
  // As big as the lap time: it is what is glanced at mid-corner.
  trkDelta = make_tyre_val(s, 0, -146);
  lv_obj_set_style_text_font(trkDelta, &lv_font_montserrat_48, 0);
  lv_obj_add_flag(trkDelta, LV_OBJ_FLAG_HIDDEN);
  trkCurrent = make_big_label(s, COL_INK, -72);
  trkLast    = make_tyre_val(s, 0, -14);
  trkBest    = make_tyre_val(s, 0, 24);
  lv_obj_set_style_text_color(trkBest, COL_GOOD, 0);
  trkStatus  = make_name(s, "", 62);
  // Both in the same centred slot; ui_update_track() shows one at a time.
  trkSetBtn  = make_button(s, "START", COL_GOOD, 0, 118, TRK_CONFIRM_SET);
  trkStopBtn = make_button(s, "STOP", COL_BAD,  0, 118, TRK_CONFIRM_STOP);
  lv_obj_add_flag(trkStopBtn, LV_OBJ_FLAG_HIDDEN);
  return s;
}

// m:ss.t or m:ss.cc. Hours are not shown: no lap is an hour long, and a
// session that reaches one has bigger problems than the display.
static void fmtLap(char *out, size_t n, uint32_t ms, int digits) {
  const unsigned m = ms / 60000UL;
  const unsigned s = (ms / 1000UL) % 60;
  if (digits == 1) snprintf(out, n, "%u:%02u.%u",   m, s, (unsigned)(ms % 1000) / 100);
  else             snprintf(out, n, "%u:%02u.%02u", m, s, (unsigned)(ms % 1000) / 10);
}

void ui_update_track(const LapInfo *li) {
  const uint32_t now = lv_tick_get();
  if (trkMsg && (int32_t)(now - trkMsgUntil) >= 0) trkMsg = NULL;
  if (trkConfirm != TRK_CONFIRM_NONE && (int32_t)(now - trkConfirmUntil) >= 0) {
    trkConfirm = TRK_CONFIRM_NONE;
  }

  // The flag shows the session: grey none, blue finding the line / out-lap,
  // green timing. Only on a state change.
  static int lastState = -1;
  if ((int)li->state != lastState) {
    lastState = (int)li->state;
    lv_obj_set_style_image_recolor(trkFlag,
        li->state == LAP_IDLE   ? COL_DIM :
        li->state == LAP_TIMING ? COL_GOOD : COL_BLUE, 0);
  }

  // The delta, in tenths - INT32_MIN for none, when the flag shows instead.
  static int32_t lastDelta = INT32_MIN + 1;         // never drawn
  const int32_t delta = li->haveDelta ? (int32_t)lroundf(li->deltaMs / 100.0f) : INT32_MIN;
  if (delta != lastDelta) {
    if ((delta == INT32_MIN) != (lastDelta == INT32_MIN) || lastDelta == INT32_MIN + 1) {
      if (delta == INT32_MIN) { lv_obj_add_flag(trkDelta, LV_OBJ_FLAG_HIDDEN); lv_obj_clear_flag(trkFlag, LV_OBJ_FLAG_HIDDEN); }
      else                    { lv_obj_clear_flag(trkDelta, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(trkFlag, LV_OBJ_FLAG_HIDDEN); }
    }
    if (delta != INT32_MIN) {
      const int32_t a = delta < 0 ? -delta : delta;
      lv_label_set_text_fmt(trkDelta, "%s%ld.%ld", delta < 0 ? "-" : delta > 0 ? "+" : "",
                            (long)(a / 10), (long)(a % 10));
      lv_obj_set_style_text_color(trkDelta, delta < 0 ? COL_GOOD : delta > 0 ? COL_BAD : COL_INK, 0);
    }
    lastDelta = delta;
  }

  // Every label below has its OWN guard - see the note in ui_priv.h.
  //
  // The running lap is shown in TENTHS. Hundredths would change every 10 ms,
  // i.e. a redraw of the big 48 px label on nearly every loop pass - the same
  // blink the oil number used to have. Completed laps are static, so they get
  // hundredths.
  static int32_t lastCur = -2;
  const int32_t cur = (li->state == LAP_TIMING) ? (int32_t)(li->currentMs / 100) : -1;
  if (cur != lastCur) {
    lastCur = cur;
    char b[16];
    if (cur < 0) snprintf(b, sizeof(b), "-:--.-");
    else         fmtLap(b, sizeof(b), (uint32_t)cur * 100, 1);
    lv_label_set_text(trkCurrent, b);
  }

  static uint32_t lastLast = 1, lastBest = 1;       // 1 = never drawn
  if (li->lastMs != lastLast) {
    lastLast = li->lastMs;
    char t[16], b[24];
    if (li->lastMs) { fmtLap(t, sizeof(t), li->lastMs, 2); snprintf(b, sizeof(b), "last %s", t); }
    else            snprintf(b, sizeof(b), "last --");
    lv_label_set_text(trkLast, b);
  }
  if (li->bestMs != lastBest) {
    lastBest = li->bestMs;
    char t[16], b[24];
    if (li->bestMs) { fmtLap(t, sizeof(t), li->bestMs, 2); snprintf(b, sizeof(b), "best %s", t); }
    else            snprintf(b, sizeof(b), "best --");
    lv_label_set_text(trkBest, b);
  }

  // START with no session, STOP with one. Same slot, one visible at a time.
  //
  // Sharing the slot is safe against a double tap: the second tap of a quick
  // double on START lands on STOP, which only ARMS its confirm - it cannot end
  // the session that the first tap just started.
  static int lastRunning = -1;
  const int running = (li->state != LAP_IDLE) ? 1 : 0;
  if (running != lastRunning) {
    lastRunning = running;
    if (running) {
      lv_obj_add_flag(trkSetBtn, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(trkStopBtn, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(trkStopBtn, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(trkSetBtn, LV_OBJ_FLAG_HIDDEN);
    }
  }

  // Status: a transient message if one is up, otherwise what to do next.
  char s[48];
  if (trkMsg) {
    snprintf(s, sizeof(s), "%s", trkMsg);
  } else {
    switch (li->state) {
      case LAP_IDLE:
        snprintf(s, sizeof(s), li->gaveUp ? "no lap found - tap START again"
                                          : "tap START, then drive");
        break;
      // Whole hundreds of metres: the label changes every 4 s at 90 km/h,
      // not on every fix.
      case LAP_SEARCH:
        if (li->searchM > 0.0f) snprintf(s, sizeof(s), "finding the line - %.1f km",
                                         (int)(li->searchM / 100.0f) / 10.0f);
        else                    snprintf(s, sizeof(s), "finding the line - waiting for GPS");
        break;
      case LAP_OUT_LAP:  snprintf(s, sizeof(s), "out lap - timing starts at the line"); break;
      // With a reference (a lap done), how far the line is - in 10 m steps
      // under a kilometre, so the label changes a few times a second at
      // most. A bullet, not a middle dot: Montserrat 14 has none.
      case LAP_TIMING: {
        const float left = calc_lap_left_m(li->refM, li->lapM);
        if (li->refM <= 0.0f) snprintf(s, sizeof(s), "lap %u", (unsigned)li->laps + 1);
        else if (left < 1000.0f)
          snprintf(s, sizeof(s), "lap %u  \xE2\x80\xA2  %d m to the line", (unsigned)li->laps + 1,
                   (int)(left / 10.0f) * 10);
        else
          snprintf(s, sizeof(s), "lap %u  \xE2\x80\xA2  %.1f km to the line", (unsigned)li->laps + 1,
                   left / 1000.0f);
        break;
      }
    }
  }

  // The arc: hidden with no reference or not timing; otherwise the part of
  // the best lap driven so far, filling up to the line. Shown or hidden only
  // on a change - clearing HIDDEN invalidates the whole object (the WiFi
  // screen's 7 Hz lesson).
  static int lastArc = -2;
  int arc = -1;
  if (li->state == LAP_TIMING && li->refM > 0.0f) {
    const float left = calc_lap_left_m(li->refM, li->lapM);
    arc = (int)floorf((li->refM - left) / li->refM * TRACK_ARC_STEPS);
    if (arc < 0) arc = 0;
    if (arc > TRACK_ARC_STEPS) arc = TRACK_ARC_STEPS;
  }
  if (arc != lastArc) {
    if ((arc < 0) != (lastArc < 0) || lastArc == -2) {
      if (arc < 0) lv_obj_add_flag(trkLeft, LV_OBJ_FLAG_HIDDEN);
      else         lv_obj_clear_flag(trkLeft, LV_OBJ_FLAG_HIDDEN);
    }
    if (arc >= 0) lv_arc_set_value(trkLeft, arc);
    lastArc = arc;
  }
  static char lastS[48] = "\x01";                   // never matches real text
  if (strcmp(s, lastS) != 0) {
    strncpy(lastS, s, sizeof(lastS) - 1);
    lastS[sizeof(lastS) - 1] = '\0';
    lv_label_set_text(trkStatus, s);
  }
}
