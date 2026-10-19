// ============================================================================
// ui_notice.cpp  —  the notification area: a coloured rim around the panel
// plus one line of text, over every screen while any fault is active.
// See ui_priv.h.
// ============================================================================
#include "ui_priv.h"
#include "power.h"      // a tap is a sign of life; hold off sleep
#include <math.h>       // cosf, sinf, floorf, fminf for the rim
#include <stdio.h>      // snprintf

static lv_obj_t *noticeRing, *noticeText;
// The rim's segments: see ui_build_notice(). lv_line keeps a pointer to its
// points, so they must outlive it; one shared style so a colour change is one
// call.
#define RIM_SEG   48
#define RIM_W     8
#define RIM_OUTER 232    // outer edge, px from the centre
static lv_point_precise_t rimPts[RIM_SEG][2];
static lv_style_t rimStyle;
static uint8_t noticeShown = 0;     // active faults being displayed (mask)
static uint8_t noticeSel   = 0;     // which one the text shows (a single bit)
static bool    noticeDrawn = false; // false until the first redraw
static int     noticeOilC  = 0;     // the temperatures the hot faults name
static int     noticeCoolC = 0;
static void noticeRedraw();
static void notice_tap_cb(lv_event_t *e);

// The notification area: a coloured rim plus one line of text, for any fault
// that should be visible from every screen - see ui_set_faults(). It started
// as a CAN-only "no link" rim; the shape is unchanged, only the colour and
// the words now depend on which fault is up.
//
// A rim rather than text in the middle. A round panel has a natural border,
// and a coloured edge reads as a state at a glance without occupying the
// middle, where the old label competed with the number beside it.
//
// On the top layer, which is the part worth keeping from the label: it has
// to show over whichever screen is loaded.
//
// Built from RIM_SEG short straight lines, not one circle. It used to be a
// bordered 462 px circle, and LVGL decides what to draw per dirty patch by
// bounding box - a full-panel box, so every patch on every screen paid for
// the border, even in the middle where no rim pixel exists. Measured: ~1 ms
// per 40x40 patch, ~25-31 ms per full redraw, +2.3 ms per g-meter frame.
// Each segment's box is a small rectangle on the edge, so patches away from
// the rim now skip it entirely. At 48 segments a chord strays half a pixel
// from the true circle - invisible.
//
// noticeRing itself is now an invisible container (no background, no
// border): it still carries the label and hides everything in one flag.
//
// Outer edge 232 px from the centre, against the outer gauge arc's 210: the
// rim sits at the very edge and the gauges are inset ~23 px, so the two do
// not read as the same thing even though both are COL_BAD when bad. 231 left
// a ring of black pixels between the chords and the panel's edge; 232 hides
// them and is still 1 px short of the panel's own rounding.
//
// The container is the full 466 px panel, not the rim's size: LVGL clips
// children to the parent's box, which shaved the stroke's outermost
// antialiased pixels when the box was exactly the rim's diameter.
//
// Colour is set by ui_set_faults(), through the shared rimStyle.
void ui_build_notice() {
  noticeRing = lv_obj_create(lv_layer_top());
  lv_obj_set_size(noticeRing, LCD_PX, LCD_PX);
  lv_obj_center(noticeRing);
  lv_obj_set_style_bg_opa(noticeRing, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(noticeRing, 0, 0);
  lv_obj_set_style_pad_all(noticeRing, 0, 0);
  {
    lv_style_init(&rimStyle);
    lv_style_set_line_width(&rimStyle, RIM_W);
    lv_style_set_line_rounded(&rimStyle, true);      // no notches at the joins
    lv_style_set_line_color(&rimStyle, COL_BAD);
    const float c = LCD_PX / 2.0f, r = RIM_OUTER - RIM_W / 2.0f;
    for (int i = 0; i < RIM_SEG; i++) {
      const float a0 = 2.0f * (float)M_PI * i / RIM_SEG;
      const float a1 = 2.0f * (float)M_PI * (i + 1) / RIM_SEG;
      const float x0 = c + r * cosf(a0), y0 = c + r * sinf(a0);
      const float x1 = c + r * cosf(a1), y1 = c + r * sinf(a1);
      // Each line object sits at its own segment's corner, so its box - and
      // therefore what LVGL considers - is just that segment plus the stroke.
      const int ox = (int)floorf(fminf(x0, x1)) - RIM_W;
      const int oy = (int)floorf(fminf(y0, y1)) - RIM_W;
      rimPts[i][0].x = x0 - ox; rimPts[i][0].y = y0 - oy;
      rimPts[i][1].x = x1 - ox; rimPts[i][1].y = y1 - oy;
      lv_obj_t *l = lv_line_create(noticeRing);
      lv_line_set_points(l, rimPts[i], 2);
      lv_obj_add_style(l, &rimStyle, 0);
      lv_obj_set_pos(l, ox, oy);
      lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);   // same reason as below
    }
  }
  // Both flags matter, and for the same reason: lv_layer_top covers the entire
  // panel, so anything here that accepts input swallows every touch on every
  // screen - swipes stop reaching the screen underneath. BOTH are set by
  // default on a new object in this LVGL (lv_obj.c constructor: CLICKABLE and
  // SCROLLABLE), so both have to be cleared explicitly.
  lv_obj_clear_flag(noticeRing, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(noticeRing, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(noticeRing, LV_OBJ_FLAG_HIDDEN);         // shown only when bad

  // What the rim actually means. The rim alone says "something is wrong", not
  // which thing, so it names itself rather than needing to be remembered.
  //
  // A CHILD of noticeRing on purpose: LVGL hides children with their parent,
  // so the two can never drift out of sync behind the one change guard in
  // ui_set_faults(). Making it a sibling would mean two objects to keep in
  // step.
  //
  // 14 px is the default font and already compiled in, so this costs no flash
  // - montserrat_16/18 are both off in lv_conf.h and enabling one would.
  //
  // 46 px up from the panel's bottom edge (the container is the full panel),
  // because the bottom of a round panel narrows fast: the chord there is
  // ~294 px against ~180 px of text. The old top banner needed the same
  // treatment for the same reason.
  noticeText = lv_label_create(noticeRing);
  lv_label_set_text(noticeText, "");
  lv_obj_set_style_text_font(noticeText, &lv_font_montserrat_14, 0);
  lv_obj_align(noticeText, LV_ALIGN_BOTTOM_MID, 0, -46);

  // Tap the text to step through the active faults. ONLY the text is
  // clickable - the rim must stay non-clickable, because it covers the whole
  // panel and would otherwise swallow every touch on every screen. With the
  // rim transparent to input, LVGL finds the label when the text is touched
  // and falls through to the screen underneath everywhere else.
  //
  // The extended click area matters: 14 px text is a small target on a panel
  // in a car, so the touchable zone reaches 20 px beyond the glyphs.
  lv_obj_add_flag(noticeText, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(noticeText, 20);
  lv_obj_add_event_cb(noticeText, notice_tap_cb, LV_EVENT_CLICKED, NULL);
}

#if FRAME_STATS
lv_obj_t *ui_notice_ring() { return noticeRing; }
#endif

void ui_set_faults(uint8_t faults, int oilC, int coolantC) {
  // Suppressed while the boot splash is up, or every cold start would flash a
  // fault across the animation before CAN or the GNSS has had a chance to
  // answer. Folded into the value being compared, so the rim appears on the
  // first call after the splash goes.
  const uint8_t shown = ui_boot_splash_up() ? 0 : faults;
  // The hot faults' text names the temperature, so a change of a degree
  // redraws too - once a degree at most, nothing for the rim's cost.
  const int oil  = (shown & FAULT_OIL_HOT)  ? oilC : 0;
  const int cool = (shown & FAULT_COOL_HOT) ? coolantC : 0;
  if (oil != noticeOilC || cool != noticeCoolC) {
    noticeOilC = oil;
    noticeCoolC = cool;
    noticeDrawn = false;
  }

  // The change guard is not an optimisation here - it is the difference
  // between a rim and a strobe.
  //
  // This is called from loop() on every iteration, i.e. a few hundred times a
  // second, and touching the rim invalidates its whole area. For a 462 px
  // circle that is essentially the entire panel, which on this partial-render
  // display means re-flushing every strip, continuously. Same lesson as the oil
  // number - see the note in ui_priv.h.
  //
  // Comparing the whole fault MASK, not just "any fault": the text and colour
  // both derive from it, so a change from one fault to another has to redraw,
  // and nothing else should. A tap redraws too, but through notice_tap_cb,
  // not here.
  if (shown == noticeShown && noticeDrawn) return;

  // A fault appearing or clearing jumps back to the most important one: a new
  // fault should be seen, not left behind whatever was tapped to last. A
  // temperature changing keeps the one tapped to.
  if (shown != noticeShown || !(noticeSel & shown)) {
    noticeShown = shown;
    noticeSel   = (uint8_t)(shown & (uint8_t)-shown);   // lowest set bit
  }
  noticeRedraw();
}

// The line of text is only one fault at a time - it is a single line at the
// bottom of a round panel, so a list would not fit - but tapping it steps to
// the next active fault. The rim stays the colour of the WORST active fault
// throughout, so tapping to a minor one can never make a lost CAN bus look
// amber; the text takes the colour of the fault it is showing.
static void noticeRedraw() {
  noticeDrawn = true;
  if (!noticeShown) {
    lv_obj_add_flag(noticeRing, LV_OBJ_FLAG_HIDDEN);
    return;
  }

  static const char *const text[] = {
    "OIL %d\xC2\xB0" "C - TOO HOT",         // FAULT_OIL_HOT
    "COOLANT %d\xC2\xB0" "C - TOO HOT",     // FAULT_COOL_HOT
    "not connected to CAN bus",           // FAULT_CAN
    "no GPS module",                      // FAULT_GPS
    "no SD card - not logging",           // FAULT_SD
  };
  int idx = 0;
  while (!(noticeSel & (1u << idx))) idx++;
  char what[40];
  snprintf(what, sizeof(what), text[idx],
           idx == 0 ? noticeOilC : idx == 1 ? noticeCoolC : 0);

  // Position among the active faults, e.g. "2/3", so it is obvious there are
  // others and that tapping moves through them. Replaces the old "+N", which
  // said others existed but gave no way to read them.
  int count = 0, pos = 0;
  for (int b = 0; b < 8; b++) {
    if (!(noticeShown & (1u << b))) continue;
    count++;
    if (b <= idx) pos = count;
  }

  // Severity by COLOUR, so the rim is readable before the text is: red for "the
  // gauge cannot do its job" (no bus, no readings), amber for "degraded" (it
  // still shows everything, but a log or a feature is missing). Otherwise a
  // missing SD card would look exactly as alarming as losing the bus.
  const lv_color_t rim  = (noticeShown & FAULTS_RED) ? COL_BAD : COL_WARN;
  const lv_color_t txtC = (noticeSel   & FAULTS_RED) ? COL_BAD : COL_WARN;
  lv_style_set_line_color(&rimStyle, rim);
  lv_obj_report_style_change(&rimStyle);    // all RIM_SEG segments at once
  lv_obj_set_style_text_color(noticeText, txtC, 0);
  if (count > 1) lv_label_set_text_fmt(noticeText, "%s  %d/%d", what, pos, count);
  else           lv_label_set_text(noticeText, what);
  lv_obj_clear_flag(noticeRing, LV_OBJ_FLAG_HIDDEN);
}

// Tap the notification text: show the next active fault, wrapping round.
static void notice_tap_cb(lv_event_t *e) {
  (void)e;
  if (touch_was_swipe()) return;         // a swipe that started on the text
  if (!noticeShown) return;
  power_note_activity();
  uint8_t next = noticeSel;
  do {
    next = (uint8_t)(next << 1);
    if (!next) next = 1;                 // wrap to the lowest bit
  } while (!(noticeShown & next));
  noticeSel = next;
  noticeRedraw();
}
