// ============================================================================
// ui.cpp  —  LVGL UI.  466x466 round AMOLED. Three arc gauges + states.
//   Uses LVGL ~9.x API. If Waveshare's example ships LVGL 8.x, the only
//   likely tweaks are lv_scr_load -> lv_screen_load and a couple of enum names.
// ============================================================================
#include "ui.h"
#include "config.h"
#include "settings.h"   // live thresholds; config.h holds only the defaults
#include "power.h"      // a screen change is a sign of life; hold off sleep
#include "laps.h"       // the track screen's buttons drive the lap timer
#include "calc.h"       // calc_lap_left_m: how far the line is
#include "lvgl.h"
#include <math.h>       // sqrtf, used by the g-meter
#include <string.h>     // strncmp/strncpy in the update guards
#include <stdio.h>      // snprintf, for the WiFi QR payload
#include <esp_heap_caps.h>   // the g-meter backdrop's canvas goes in PSRAM

// Top-down car for the tyre screen. car_img.c is generated C, so the C++ side
// needs C linkage - plain LV_IMAGE_DECLARE would look for a mangled name.
extern "C" const lv_image_dsc_t car_top;
// Oil-temperature and coolant-temperature warning-light symbols for the oil
// screen: A8 masks (icon_oil.c, icon_coolant.c), coloured on the device.
extern "C" const lv_image_dsc_t icon_oil;
extern "C" const lv_image_dsc_t icon_coolant;
extern "C" const lv_image_dsc_t icon_turbo;      // boost screen
extern "C" const lv_image_dsc_t icon_sat;        // GPS screen
extern "C" const lv_image_dsc_t icon_flag;       // track screen
// Hyundai N badge for the boot splash (n_logo_img.c), full colour: the
// badge's near-black outline and shadow are lifted to grey in the source PNG
// (firmware/assets/hyundai_n_logo.png), or they vanish on the dark panel.
extern "C" const lv_image_dsc_t n_logo;

#if FRAME_STATS
#include <Arduino.h>        // micros, Serial - ui_bench() only
#include <esp_task_wdt.h>
#endif

// palette (matches the guide's instrument look)
#define COL_BG      lv_color_hex(0x0d1116)
#define COL_OIL     lv_color_hex(0xE8A33D)
#define COL_BLUE    lv_color_hex(0x17A9E6)
#define COL_GOOD    lv_color_hex(0x4CC38A)
#define COL_BAD     lv_color_hex(0xE5544B)
// Amber for "degraded, still working" notices. The same hex as COL_OIL, but
// named for what it MEANS at the use site: a notice that is amber because it is
// a warning should not change if the oil gauge's accent colour ever does.
#define COL_WARN    lv_color_hex(0xE8A33D)
#define COL_INK     lv_color_hex(0xE7EDF3)
#define COL_LINE    lv_color_hex(0x26313C)
// A symbol with no reading behind it: clearly there, clearly not live.
// COL_LINE is too close to the background to read as anything.
#define COL_DIM     lv_color_hex(0x5A6673)

static GaugeView s_view = VIEW_OIL;
static lv_obj_t *scr[VIEW_COUNT];

// oil widgets
static lv_obj_t *oilArc, *oilVal, *oilState, *oilIcon;
static lv_obj_t *coolIcon;
// coolant widgets
static lv_obj_t *coolArc, *coolVal, *coolState;
// boost widgets
static lv_obj_t *boostArc, *boostVal, *boostIcon;
static lv_obj_t *torqueArc, *torqueVal, *pedalBar;
// gmeter widgets. gTrail[0] is the newest history dot, [N-1] the oldest.
static lv_obj_t *gDot, *gLabel;
static lv_obj_t *gTrail[GMETER_TRAIL_N];
// tpms widgets
static lv_obj_t *tyreVal[WHEEL_COUNT], *tyreTemp[WHEEL_COUNT];
// gps widgets
static lv_obj_t *gpsSpeed, *gpsState, *gpsTrip, *gpsIcon;
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
// wifi widgets
static lv_obj_t *wifiSsid, *wifiPass, *wifiIp, *wifiQr, *wifiClients, *wifiArc, *wifiTitle;
// shared
// Notification area: a coloured rim around the panel plus one line of text,
// shown over every screen while any fault is active. See ui_set_faults().
static lv_obj_t *noticeRing, *noticeText;
// The rim's segments: see ui_init(). lv_line keeps a pointer to its points,
// so they must outlive it; one shared style so a colour change is one call.
#define RIM_SEG   48
#define RIM_W     8
#define RIM_OUTER 232    // outer edge, px from the centre
#define LCD_PX    466    // the panel, square and round
static lv_point_precise_t rimPts[RIM_SEG][2];
static lv_style_t rimStyle;
static uint8_t noticeShown = 0;     // active faults being displayed (mask)
static uint8_t noticeSel   = 0;     // which one the text shows (a single bit)
static bool    noticeDrawn = false; // false until the first redraw
static int     noticeOilC  = 0;     // the temperatures the hot faults name
static int     noticeCoolC = 0;
static void noticeRedraw();
static void notice_tap_cb(lv_event_t *e);

// --- helpers ----------------------------------------------------------------
// Swipe to change screens: left for the next one, right for the previous one.
// (The hardware button only wakes the gauge, since 2026-10-13.)
//
// It used to be a tap anywhere, which meant a tap could never mean anything
// else - no screen could have a button without also cycling away from itself.
// Swiping frees taps for buttons (the track screen's, first).
//
// The gesture arrives here from anywhere on the screen, children included:
// LVGL bubbles it up through every object carrying LV_OBJ_FLAG_GESTURE_BUBBLE,
// which lv_obj_create sets on anything WITH a parent - and stops at the screen,
// which has none. Vertical swipes are ignored, so they stay free for later use.
static void screen_gesture_cb(lv_event_t *e) {
  (void)e;
  lv_indev_t *indev = lv_indev_active();
  const lv_dir_t dir = lv_indev_get_gesture_dir(indev);
  if (dir != LV_DIR_LEFT && dir != LV_DIR_RIGHT) return;
  ui_step_view(dir == LV_DIR_LEFT ? +1 : -1);
  // One swipe, one screen: the finger is still down, and on the new screen
  // LVGL takes what is under it for a fresh press - the rest of the same
  // swipe would be a second gesture, and a third. Ignored until it lifts.
  lv_indev_wait_release(indev);
}

// For button handlers: was the touch that just ended actually a swipe?
//
// Needed because LVGL sends LV_EVENT_CLICKED on release whenever nothing
// SCROLLED, whether or not a gesture fired (lv_indev.c, release path). So a
// swipe that happens to start on a button would also click it - on the track
// screen, a swipe across it could set the start line. The gesture direction is
// reset to LV_DIR_NONE on every new press, so it reliably describes this touch.
static bool touch_was_swipe() {
  return lv_indev_get_gesture_dir(lv_indev_active()) != LV_DIR_NONE;
}

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

// "Share SD card with PC" on the WiFi screen - see ui_update_usbdisk().
static lv_obj_t *shareBtn, *shareLbl;
static volatile bool shareTapped = false;

static void share_btn_cb(lv_event_t *e) {
  (void)e;
  if (touch_was_swipe()) return;         // a swipe that started on the button
  power_note_activity();
  shareTapped = true;                    // acted on from loop(), not from LVGL
}

// Any press on the WiFi screen restarts its countdown - see ui_take_wifi_touch().
static volatile bool wifiTouched = false;

static void wifi_touch_cb(lv_event_t *e) {
  (void)e;
  wifiTouched = true;                    // acted on from loop(), not from LVGL
}

static lv_obj_t* make_screen() {
  lv_obj_t *s = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(s, COL_BG, 0);
  lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);   // a press must land somewhere
  lv_obj_add_event_cb(s, screen_gesture_cb, LV_EVENT_GESTURE, NULL);
  return s;
}

static lv_obj_t* make_arc_sized(lv_obj_t *parent, lv_color_t c, int min, int max,
                                int px, int width) {
  lv_obj_t *a = lv_arc_create(parent);
  lv_obj_set_size(a, px, px);
  lv_obj_center(a);
  lv_arc_set_rotation(a, 135);
  lv_arc_set_bg_angles(a, 0, 270);
  lv_arc_set_range(a, min, max);
  lv_obj_remove_style(a, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_color(a, COL_LINE, LV_PART_MAIN);
  lv_obj_set_style_arc_width(a, width, LV_PART_MAIN);
  lv_obj_set_style_arc_color(a, c, LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(a, width, LV_PART_INDICATOR);
  return a;
}

static lv_obj_t* make_arc(lv_obj_t *parent, lv_color_t c, int min, int max) {
  lv_obj_t *a = lv_arc_create(parent);
  lv_obj_set_size(a, 420, 420);
  lv_obj_center(a);
  lv_arc_set_rotation(a, 135);
  lv_arc_set_bg_angles(a, 0, 270);
  lv_arc_set_range(a, min, max);
  lv_obj_remove_style(a, NULL, LV_PART_KNOB);          // no drag knob
  lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_color(a, COL_LINE, LV_PART_MAIN);
  lv_obj_set_style_arc_width(a, 16, LV_PART_MAIN);
  lv_obj_set_style_arc_color(a, c, LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(a, 16, LV_PART_INDICATOR);
  return a;
}

static lv_obj_t* make_big_label(lv_obj_t *parent, lv_color_t c, int dy) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_color(l, c, 0);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_48, 0);  // enable in lv_conf.h
  lv_label_set_text(l, "--");
  lv_obj_align(l, LV_ALIGN_CENTER, 0, dy);
  return l;
}


static lv_obj_t* make_tyre_val(lv_obj_t *parent, int dx, int dy) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_color(l, COL_INK, 0);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
  lv_label_set_text(l, "--");
  lv_obj_align(l, LV_ALIGN_CENTER, dx, dy);
  return l;
}

// A centred row of [symbol][value]: the caller creates the value label inside
// the returned row. A flex row rather than aligning the symbol to the label,
// because the label's width changes with the number ("9°" vs "105°") and
// lv_obj_align_to() does not follow that; the row re-lays itself out.
// The symbol is an A8 mask, drawn in its image_recolor colour - set that to
// show state.
static lv_obj_t* make_icon_row(lv_obj_t *parent, int dy,
                               const lv_image_dsc_t *icon, lv_obj_t **iconOut) {
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);                 // no background, border, padding
  lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, 10, 0);
  lv_obj_align(row, LV_ALIGN_CENTER, 0, dy);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *img = lv_image_create(row);
  lv_image_set_src(img, icon);
  lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
  lv_obj_set_style_image_recolor(img, COL_DIM, 0);   // no data until told
  lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
  *iconOut = img;
  return row;
}

// Pin a row's value label to the width of the widest thing it can show, text
// left-aligned. Without this the row re-centred on every value - "0.95" is
// narrower than "1.23", "--" narrower still - and the symbol beside it jumped
// sideways with each sample. Call after the label's font is set.
//
// `pattern` is the value's shape with every digit written as '8'. Each digit
// 0-9 is tried in turn and the widest kept: digits are NOT all the same width
// in Montserrat, and sizing for "8.88" alone let "0.00" overflow.
// CLIP rather than the default WRAP, so a value that somehow still does not
// fit can never become a second line - that changed the label's height, the
// row re-centred vertically, and the symbol jumped all the same.
static void fix_value_width(lv_obj_t *label, const char *pattern) {
  const lv_font_t *font = lv_obj_get_style_text_font(label, 0);
  char buf[16];
  int32_t widest = 0;
  for (char d = '0'; d <= '9'; d++) {
    size_t i = 0;
    for (; pattern[i] && i < sizeof(buf) - 1; i++)
      buf[i] = pattern[i] == '8' ? d : pattern[i];
    buf[i] = '\0';
    lv_point_t sz;
    lv_text_get_size(&sz, buf, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (sz.x > widest) widest = sz.x;
  }
  lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
  lv_obj_set_width(label, widest);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
}

// "No value" in a fixed-width slot: "--" CENTRED and dim. Left-aligned like a
// reading, the narrow "--" hugged the symbol with the rest of the slot empty
// beside it, so the symbol and dashes sat visibly off-centre (2026-10-09) -
// and in the value's colour they read as a value, not as "nothing yet".
static void show_no_value(lv_obj_t *label) {
  lv_label_set_text(label, "--");
  lv_obj_set_style_text_color(label, COL_DIM, 0);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
}
// Back to a reading: left-aligned (see fix_value_width), in its colour.
static void show_value_style(lv_obj_t *label, lv_color_t c) {
  lv_obj_set_style_text_color(label, c, 0);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
}

// Whether the car is answering at all - says WHY a value is missing on the
// oil screen's state line. Set from loop() via ui_set_car_talking().
static bool carTalking = false;

// A plain circle used as a g-meter dot. Non-clickable so taps still cycle.
static lv_obj_t* make_dot(lv_obj_t *parent, int d, lv_color_t c, lv_opa_t opa) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_set_size(o, d, d);
  lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(o, c, 0);
  lv_obj_set_style_bg_opa(o, opa, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_center(o);
  return o;
}

// The g-meter's backdrop: the full-scale and half-scale rings, plus an equator
// and meridian as squashed ellipses (LVGL has no 3D and no ellipse primitive),
// which together read as a globe rather than a target.
//
// Drawn ONCE into a canvas instead of kept as live arc and line objects.
// Measured on this panel: those four objects were 45 ms of a 108 ms full
// redraw, and every 40x40 patch the moving dot dirtied cost ~4 ms against
// ~0.7 ms on a screen without them - LVGL's software renderer is slow at large
// curves, and a patch still pays for every curve that passes through it. As a
// pre-rendered opaque image, a patch is a row copy.
//
// The buffer (424x424 RGB565, ~350 KB) lives in PSRAM; internal RAM could not
// spare it. If the allocation fails, the screen simply has no backdrop.
#define GLOBE_SEG  48
#define GM_BG_SIZE 424                 // just clears the 420 px outer ring

static void make_gmeter_backdrop(lv_obj_t *parent) {
  const uint32_t bytes = LV_CANVAS_BUF_SIZE(GM_BG_SIZE, GM_BG_SIZE, 16,
                                            LV_DRAW_BUF_STRIDE_ALIGN);
  void *buf = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
  if (!buf) return;
  lv_obj_t *cv = lv_canvas_create(parent);
  lv_canvas_set_buffer(cv, buf, GM_BG_SIZE, GM_BG_SIZE, LV_COLOR_FORMAT_RGB565);
  lv_canvas_fill_bg(cv, COL_BG, LV_OPA_COVER);
  lv_obj_center(cv);
  lv_obj_clear_flag(cv, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(cv, LV_OBJ_FLAG_SCROLLABLE);

  lv_layer_t layer;
  lv_canvas_init_layer(cv, &layer);
  const int c = GM_BG_SIZE / 2;

  // Outer limit (= GMETER_FS_G) and a mid ring at half scale, for reference.
  lv_draw_arc_dsc_t arc;
  lv_draw_arc_dsc_init(&arc);
  arc.color = COL_LINE;
  arc.width = 2;
  arc.center.x = c; arc.center.y = c;
  arc.start_angle = 0; arc.end_angle = 360;
  arc.radius = 210; lv_draw_arc(&layer, &arc);
  arc.radius = 105; lv_draw_arc(&layer, &arc);

  // Equator and meridian.
  lv_draw_line_dsc_t ln;
  lv_draw_line_dsc_init(&ln);
  ln.color = COL_LINE;
  ln.width = 2;
  ln.round_start = ln.round_end = 1;   // no notches where segments meet
  static const float radii[2][2] = { { 210.0f, 74.0f }, { 74.0f, 210.0f } };
  for (int e = 0; e < 2; e++) {
    for (int i = 0; i < GLOBE_SEG; i++) {
      const float a0 = 2.0f * (float)M_PI * i / GLOBE_SEG;
      const float a1 = 2.0f * (float)M_PI * (i + 1) / GLOBE_SEG;
      ln.p1.x = c + radii[e][0] * cosf(a0); ln.p1.y = c + radii[e][1] * sinf(a0);
      ln.p2.x = c + radii[e][0] * cosf(a1); ln.p2.y = c + radii[e][1] * sinf(a1);
      lv_draw_line(&layer, &ln);
    }
  }
  lv_canvas_finish_layer(cv, &layer);
}

static lv_obj_t* make_name(lv_obj_t *parent, const char *txt, int dy) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_color(l, COL_INK, 0);
  lv_label_set_text(l, txt);
  lv_obj_align(l, LV_ALIGN_CENTER, 0, dy);
  return l;
}

// --- build ------------------------------------------------------------------
void ui_init() {
  // TEMPS: oil and coolant on ONE screen, as concentric arcs. They are read
  // together, they fail together, and flipping between two nearly identical
  // dials while driving was more work than it was worth. Oil is the outer
  // arc and the big number because it is the one this gauge exists for;
  // coolant is the inner arc and a smaller figure under it.
  scr[VIEW_OIL] = make_screen();
  oilArc   = make_arc_sized(scr[VIEW_OIL], COL_OIL,  0, 150, 420, 16);
  coolArc  = make_arc_sized(scr[VIEW_OIL], COL_BLUE, 0, 130, 372, 12);
  // Each reading is its dashboard symbol beside the number, in place of the
  // old "OIL / COOLANT" caption - the same symbols as the car's own warning
  // lights, so they need no reading. Coloured by state in the updaters.
  lv_obj_t *oilRow  = make_icon_row(scr[VIEW_OIL], -34, &icon_oil,     &oilIcon);
  lv_obj_t *coolRow = make_icon_row(scr[VIEW_OIL],  16, &icon_coolant, &coolIcon);
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
  oilState = make_name(scr[VIEW_OIL], "", 84);
  // Coolant has no separate state line any more - an over-temp coolant shows
  // as a red inner arc, and the oil state line carries the wording.
  coolState = oilState;

  // BOOST, with engine torque inside it - the same two-ring layout as the
  // temps screen. Torque is an ESTIMATE from absolute load (no torque value
  // exists at the OBD port), so its figure says "~" and moves in 5 Nm steps.
  scr[VIEW_BOOST] = make_screen();
  boostArc = make_arc(scr[VIEW_BOOST], COL_BLUE, 0, 200);  // 0..2.00 bar *100
  torqueArc = make_arc_sized(scr[VIEW_BOOST], COL_INK, 0, TORQUE_ARC_MAX_NM, 372, 12);
  torqueVal = make_tyre_val(scr[VIEW_BOOST], 0, 84);        // 28px, under "bar"
  // Accelerator pedal: a short bar in the open bottom quarter both arcs
  // leave (their ends sit ~148 px below centre, so 175 clears them and stays
  // inside the round panel).
  pedalBar = lv_bar_create(scr[VIEW_BOOST]);
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
  lv_obj_t *boostRow = make_icon_row(scr[VIEW_BOOST], -10, &icon_turbo, &boostIcon);
  boostVal = make_big_label(boostRow, COL_BLUE, 0);
  fix_value_width(boostVal, "8.88");                // ui_update_boost clamps < 0 to 0
  make_name(scr[VIEW_BOOST], "bar", 40);

  // G-METER: a faked sphere with a g-g trail across it.
  scr[VIEW_GMETER] = make_screen();
  make_gmeter_backdrop(scr[VIEW_GMETER]);

  // Trail: oldest drawn first so the newest sits on top. Older = smaller and
  // more transparent, which is what makes the direction of travel readable.
  for (int i = GMETER_TRAIL_N - 1; i >= 0; i--) {
    const float age = (float)i / GMETER_TRAIL_N;         // 0 = newest
    gTrail[i] = make_dot(scr[VIEW_GMETER], 8 + (int)(8 * (1.0f - age)),
                         COL_BLUE, (lv_opa_t)(40 + 160 * (1.0f - age)));
  }
  gDot   = make_dot(scr[VIEW_GMETER], 26, COL_GOOD, LV_OPA_COVER);
  gLabel = make_name(scr[VIEW_GMETER], "G-FORCE", 150);

  // TYRES - the car seen from above, a wheel marker at each corner and its
  // pressure and temperature beside it. 466px round panel, so keep it well
  // inside.
  scr[VIEW_TPMS] = make_screen();
  // The car, seen from above, behind everything else on the screen: created
  // first so the wheels and numbers draw over it. A compiled-in image (see
  // car_img.c, generated by scripts/png2lvgl.py), copied row by row - as
  // cheap to redraw as the g-meter backdrop.
  {
    lv_obj_t *car = lv_image_create(scr[VIEW_TPMS]);
    lv_image_set_src(car, &car_top);
    lv_obj_center(car);
    lv_obj_clear_flag(car, LV_OBJ_FLAG_CLICKABLE);
  }
  // Where each wheel is on the car image: its body is ~+-64 px wide (the
  // height limits the scaling, so it is narrower than the 200 px box), so a
  // wheel sits just outside at +-79, axles at the usual ~20% / ~25% of the
  // length from nose and tail - the wheels themselves are hidden in a top
  // view. No wheel markers are drawn any more (the picture is enough, and the
  // pressure's colour carries the warning); these only place the readings,
  // OUTBOARD of each wheel, off the car, where the round panel still has
  // ~210 px of half-width at these heights.
  static const int tx[WHEEL_COUNT] = {  -79,   79,  -79,   79 };
  static const int ty[WHEEL_COUNT] = {  -80,  -80,   72,   72 };
  for (int i = 0; i < WHEEL_COUNT; i++) {
    const int lx = tx[i] > 0 ? tx[i] + 56 : tx[i] - 56;
    tyreVal[i] = make_tyre_val(scr[VIEW_TPMS], lx, ty[i]);
    // Temperature below the pressure. Was the default ~14 px font, which was
    // reported as too small to read at a glance while driving - the whole
    // point of a dash gauge - so it now uses the same 28 px face as the
    // pressure, in blue to keep the two readings distinguishable.
    tyreTemp[i] = lv_label_create(scr[VIEW_TPMS]);
    lv_obj_set_style_text_color(tyreTemp[i], COL_BLUE, 0);
    lv_obj_set_style_text_font(tyreTemp[i], &lv_font_montserrat_28, 0);
    lv_label_set_text(tyreTemp[i], "");
    lv_obj_align(tyreTemp[i], LV_ALIGN_CENTER, lx, ty[i] + 30);
  }
  // No "TYRES" caption: it sat over the car's roof, and the car picture
  // already says what this screen is.

  // GPS
  scr[VIEW_GPS] = make_screen();
  // Satellite beside the speed: says "this is GPS speed" in place of the old
  // "(GPS)", and its colour is the fix state at a glance.
  lv_obj_t *gpsRow = make_icon_row(scr[VIEW_GPS], -10, &icon_sat, &gpsIcon);
  gpsSpeed = make_big_label(gpsRow, COL_GOOD, 0);
  fix_value_width(gpsSpeed, "888");
  make_name(scr[VIEW_GPS], "km/h", 40);
  gpsState = make_name(scr[VIEW_GPS], "", 80);
  // Below the fix state rather than beside the speed: at 466 px the speed is
  // already the full chord width, and crowding a second number next to it
  // would shrink both.
  gpsTrip  = make_tyre_val(scr[VIEW_GPS], 0, 120);

  // TRACK: lap timer. Current lap big in the middle, last and best under it,
  // a status line saying what to do next, and the two buttons.
  //
  // The buttons sit at +118, not lower: the notification rim's text line
  // occupies roughly +163..+179 on this round panel, and below +150 the chord
  // also narrows too fast for two 130 px buttons side by side.
  scr[VIEW_TRACK] = make_screen();
  // How far the line is, against the best lap's length: an arc round the
  // edge filling up towards it - full at the line. First, so it
  // is under everything - and not clickable (make_arc_sized), so the
  // buttons above it still take every tap.
  trkLeft = make_arc_sized(scr[VIEW_TRACK], COL_BLUE, 0, TRACK_ARC_STEPS, 440, 12);
  lv_obj_add_flag(trkLeft, LV_OBJ_FLAG_HIDDEN);
  // Chequered flag in place of the "LAP TIMER" caption (Material Symbols
  // "sports_score", Apache 2.0 - see firmware/assets/). Coloured by session
  // state in ui_update_track().
  trkFlag = lv_image_create(scr[VIEW_TRACK]);
  lv_image_set_src(trkFlag, &icon_flag);
  lv_obj_set_style_image_recolor_opa(trkFlag, LV_OPA_COVER, 0);
  lv_obj_set_style_image_recolor(trkFlag, COL_DIM, 0);
  lv_obj_align(trkFlag, LV_ALIGN_CENTER, 0, -150);
  lv_obj_clear_flag(trkFlag, LV_OBJ_FLAG_CLICKABLE);
  // The live delta to the best lap, in the flag's place while there is one
  // (LapInfo.haveDelta): "-1.2" green, ahead; "+0.8" red, behind.
  // As big as the lap time: it is what is glanced at mid-corner.
  trkDelta = make_tyre_val(scr[VIEW_TRACK], 0, -146);
  lv_obj_set_style_text_font(trkDelta, &lv_font_montserrat_48, 0);
  lv_obj_add_flag(trkDelta, LV_OBJ_FLAG_HIDDEN);
  trkCurrent = make_big_label(scr[VIEW_TRACK], COL_INK, -72);
  trkLast    = make_tyre_val(scr[VIEW_TRACK], 0, -14);
  trkBest    = make_tyre_val(scr[VIEW_TRACK], 0, 24);
  lv_obj_set_style_text_color(trkBest, COL_GOOD, 0);
  trkStatus  = make_name(scr[VIEW_TRACK], "", 62);
  // Both in the same centred slot; ui_update_track() shows one at a time.
  trkSetBtn  = make_button(scr[VIEW_TRACK], "START", COL_GOOD, 0, 118, TRK_CONFIRM_SET);
  trkStopBtn = make_button(scr[VIEW_TRACK], "STOP", COL_BAD,  0, 118, TRK_CONFIRM_STOP);
  lv_obj_add_flag(trkStopBtn, LV_OBJ_FLAG_HIDDEN);

  // WIFI: how to reach the settings panel. The passphrase is generated per
  // device and stored in NVS, so this screen is the only place it can be read
  // - there is no default printed in the source and no reachable button.
  scr[VIEW_WIFI] = make_screen();
  // Laid out top to bottom; the "Share SD card" button replaces the title
  // while a PC is connected. The QR is 130 px - still a few pixels per
  // module for this short payload.
  wifiTitle = make_name(scr[VIEW_WIFI], "SCAN TO JOIN", -185);
  // Countdown to the automatic return to the gauges (WIFI_VIEW_MAX_MS): full
  // on arrival, empty at the switch - see ui_update_wifi_countdown(). Thin,
  // so it frames the screen rather than reading as a gauge.
  wifiArc = make_arc_sized(scr[VIEW_WIFI], COL_BLUE, 0, WIFI_ARC_STEPS, 440, 6);
  lv_arc_set_value(wifiArc, WIFI_ARC_STEPS);
  // A touch anywhere on it means someone is still reading: start over.
  lv_obj_add_event_cb(scr[VIEW_WIFI], wifi_touch_cb, LV_EVENT_PRESSED, NULL);

  // The QR carries a standard WIFI: payload, so a phone camera joins the
  // network without anyone typing a random 10-character passphrase. White
  // background with a quiet zone: a code drawn straight onto the dark theme
  // usually will not scan, and the border is part of the spec, not padding.
  wifiQr = lv_qrcode_create(scr[VIEW_WIFI]);
  lv_qrcode_set_size(wifiQr, 130);
  lv_qrcode_set_dark_color(wifiQr, lv_color_black());
  lv_qrcode_set_light_color(wifiQr, lv_color_white());
  lv_obj_set_style_border_color(wifiQr, lv_color_white(), 0);
  lv_obj_set_style_border_width(wifiQr, 6, 0);
  lv_obj_align(wifiQr, LV_ALIGN_CENTER, 0, -58);   // -129..13 with the border
  lv_obj_clear_flag(wifiQr, LV_OBJ_FLAG_CLICKABLE);   // taps still cycle views

  // Network name first, then the passphrase: that is the order they are asked
  // for, and the order they read in. Both in text as well as in the code,
  // because scanning fails often enough - dirty lens, sun on an AMOLED - that
  // the fallback has to be readable.
  wifiSsid = make_tyre_val(scr[VIEW_WIFI], 0, 38);
  lv_obj_set_style_text_color(wifiSsid, COL_OIL, 0);
  wifiPass = make_tyre_val(scr[VIEW_WIFI], 0, 70);
  lv_obj_set_style_text_color(wifiPass, COL_INK, 0);
  wifiIp   = make_name(scr[VIEW_WIFI], "", 100);
  lv_obj_set_style_text_color(wifiIp, COL_BLUE, 0);
  // Connected clients, which doubles as confirmation that a phone actually
  // associated - useful when a join silently fails.
  wifiClients = make_name(scr[VIEW_WIFI], "", 122);
  lv_obj_set_style_text_color(wifiClients, COL_LINE, 0);

  // Lend the SD card to a PC as a USB drive (usbdisk.cpp). Only shown while a
  // PC is connected, i.e. on the bench, and then in the title's place at the
  // top: at the bottom (+140) it sat against the fault notice, which shows
  // "no CAN bus" on the bench and took the taps. As big as fits for a
  // finger: 190x44 at -171 spans -193..-149, 20 px above the QR, with
  // its rounded ends (r <= 208) inside the countdown arc (r 214..220); the
  // touch area reaches 8 px further, over parts that take no taps.
  shareBtn = lv_button_create(scr[VIEW_WIFI]);
  lv_obj_set_size(shareBtn, 190, 44);
  lv_obj_align(shareBtn, LV_ALIGN_CENTER, 0, -171);
  lv_obj_set_ext_click_area(shareBtn, 8);
  lv_obj_set_style_radius(shareBtn, 22, 0);
  lv_obj_set_style_bg_color(shareBtn, COL_BG, 0);
  lv_obj_set_style_bg_color(shareBtn, COL_LINE, LV_STATE_PRESSED);
  lv_obj_set_style_border_color(shareBtn, COL_BLUE, 0);
  lv_obj_set_style_border_width(shareBtn, 2, 0);
  lv_obj_set_style_shadow_width(shareBtn, 0, 0);
  lv_obj_clear_flag(shareBtn, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(shareBtn, share_btn_cb, LV_EVENT_CLICKED, NULL);
  shareLbl = lv_label_create(shareBtn);
  lv_obj_set_style_text_font(shareLbl, &lv_font_montserrat_14, 0);
  lv_obj_clear_flag(shareLbl, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_center(shareLbl);
  lv_obj_add_flag(shareBtn, LV_OBJ_FLAG_HIDDEN);

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

  // A swipe that STARTS on the text must still change screen. Its gesture
  // bubbles label -> rim -> lv_layer_top, and stops there because the top
  // layer has no parent - so it would reach no screen at all. The top layer
  // gets the same handler the screens have.
  lv_obj_add_event_cb(lv_layer_top(), screen_gesture_cb, LV_EVENT_GESTURE, NULL);

  // No AP banner. It existed to announce a briefly-toggled AP; the AP is now
  // up the whole time the board is awake, so a permanent badge on every screen
  // is just noise - and VIEW_WIFI carries the name, passphrase and URL.

  lv_scr_load(scr[VIEW_OIL]);
}

// --- boot animation ---------------------------------------------------------
// A splash on its own screen: the two arcs sweep up from zero while the name
// fades in, then it hands over to the gauges. Built and destroyed here rather
// than living in a view, so it costs nothing after boot.
//
// It is deliberately NOT a fixed delay. ui_boot_anim_done() reports when the
// sweep has finished so the caller can carry on driving the loop meanwhile -
// CAN, GPS and the watchdog all keep running through it. Blocking for a
// second and a half of animation would drop frames from the GNSS UART and
// risk the 8 s watchdog on a slow SD mount.
static lv_obj_t *bootScr = NULL;
static uint32_t  bootStartMs = 0;
static bool      bootTimerRunning = false;

static void boot_arc_exec(void *obj, int32_t v) {
  lv_arc_set_value((lv_obj_t *)obj, v);
}
// Slides the name up into place. Animating POSITION rather than opacity is
// deliberate: the first version faded text in from LV_OPA_TRANSP, and when
// that animation didn't take effect the label stayed fully transparent, i.e.
// invisible with no clue why. A position animation that fails just leaves the
// text sitting at its start offset - still readable.
static void boot_y_exec(void *obj, int32_t v) {
  lv_obj_align((lv_obj_t *)obj, LV_ALIGN_CENTER, 0, v);
}

static lv_obj_t *bootOuter, *bootInner, *bootName;
static lv_obj_t *bootBench = NULL;    // the status text, once on the bench
static volatile bool bootTouched = false;

// After the sweep the splash is held (see the loop), and a touch lets it go.
static void boot_press_cb(lv_event_t *e) {
  (void)e;
  bootTouched = true;                 // acted on from loop(), not from LVGL
}

void ui_boot_anim_start() {
  bootScr          = make_screen();
  bootTimerRunning = false;   // timer AND animations start on the first
                              // ui_boot_anim_done() call, i.e. from loop()
  lv_obj_clear_flag(bootScr, LV_OBJ_FLAG_CLICKABLE);   // taps must not skip it

  // Same geometry as the temps screen, so the handover looks continuous
  // rather than like two unrelated designs.
  bootOuter = make_arc_sized(bootScr, COL_OIL,  0, 100, 420, 16);
  bootInner = make_arc_sized(bootScr, COL_BLUE, 0, 100, 372, 12);
  lv_arc_set_value(bootOuter, 0);
  lv_arc_set_value(bootInner, 0);

  // The N badge, where "i30 N" in Montserrat used to be. 280x118, so its
  // corners stay ~30 px inside the inner arc. Starts low and slides up to
  // dead centre.
  bootName = lv_image_create(bootScr);
  lv_image_set_src(bootName, &n_logo);
  lv_obj_clear_flag(bootName, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(bootName, LV_ALIGN_CENTER, 0, 22);

  lv_scr_load(bootScr);

  // Paint it NOW, rather than leaving it to the first lv_timer_handler() in
  // loop(). This is the fix for "black for several seconds on buck power".
  //
  // lv_scr_load() only marks a screen active; nothing reaches the panel until
  // a refresh runs, and that otherwise happens only in loop(). So the whole
  // remainder of setup() - CAN, the GNSS wake burst, the SD mount, the
  // watchdog, bringing up the AP - used to run against a black display, and
  // the splash appeared only once loop() began. The comment at the call site
  // claimed "splash while the rest of setup() runs", which was never true.
  //
  // lv_refr_now(), not lv_timer_handler(): the refresh timer has a 33 ms
  // period (LV_DEF_REFR_PERIOD), so a handler call this soon after the load
  // finds it not yet due and paints nothing.
  //
  // Only the static frame. The animation still starts from the first
  // ui_boot_anim_done() call in loop(), so a slow setup() cannot eat its
  // window - which is the bug that guard exists for.
  lv_refr_now(NULL);
}

void ui_show_notice(const char *msg) {
  // On lv_layer_top, so it shows over the splash or any gauge screen - the
  // thing this reports can happen at either point.
  static lv_obj_t *notice = NULL;
  if (msg == NULL) {                          // NULL hides it again
    if (notice) { lv_obj_add_flag(notice, LV_OBJ_FLAG_HIDDEN); lv_refr_now(NULL); }
    return;
  }
  if (!notice) {
    notice = lv_label_create(lv_layer_top());
    lv_obj_set_style_text_color(notice, COL_INK, 0);
    lv_obj_set_style_text_font(notice, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(notice, COL_BG, 0);
    lv_obj_set_style_bg_opa(notice, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(notice, 8, 0);
    lv_label_set_long_mode(notice, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(notice, 300);           // chord at the middle, with margin
    lv_obj_set_style_text_align(notice, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(notice);
    lv_obj_clear_flag(notice, LV_OBJ_FLAG_CLICKABLE);
  }
  lv_label_set_text(notice, msg);
  lv_obj_clear_flag(notice, LV_OBJ_FLAG_HIDDEN);
  lv_refr_now(NULL);                          // not lv_timer_handler; see below
}


// Kicked off from loop(), not from setup(). LVGL animations advance on
// lv_tick, and lv_timer_handler() only runs in loop() - so animations started
// during setup() would have already elapsed by the time the first frame was
// drawn, and the arcs would appear pinned at full scale.
static void bootAnimsStart() {
  // Outer sweeps first, inner chases it - reads as the gauge "spooling up".
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, bootOuter);
  lv_anim_set_exec_cb(&a, boot_arc_exec);
  lv_anim_set_values(&a, 0, 100);
  lv_anim_set_duration(&a, BOOT_ANIM_MS * 2 / 3);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);

  lv_anim_set_var(&a, bootInner);
  lv_anim_set_delay(&a, BOOT_ANIM_MS / 5);
  lv_anim_start(&a);

  lv_anim_init(&a);
  lv_anim_set_var(&a, bootName);
  lv_anim_set_exec_cb(&a, boot_y_exec);
  lv_anim_set_values(&a, 22, 0);          // same 22 px of travel as before
  lv_anim_set_duration(&a, BOOT_ANIM_MS / 2);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);
}

bool ui_boot_anim_done() {
  if (bootScr == NULL) return true;
  // Time the splash from the first call, not from when it was built.
  // ui_boot_anim_start() runs inside setup(), where lv_timer_handler() is
  // never called - so nothing renders and no animation advances until loop()
  // begins. Timing from creation meant a long init (GPS baud hunt, SD mount)
  // burned the whole window before a single frame was drawn, and the splash
  // appeared for one frame and vanished. This call only happens from loop(),
  // which is exactly when drawing starts.
  if (!bootTimerRunning) {
    bootTimerRunning = true;
    bootStartMs      = lv_tick_get();
    bootAnimsStart();
    return false;
  }
  if ((lv_tick_get() - bootStartMs) < BOOT_ANIM_MS) return false;
  // Swept: from now on a touch may end it (the loop holds it up otherwise).
  if (!lv_obj_has_flag(bootScr, LV_OBJ_FLAG_CLICKABLE)) {
    lv_obj_add_flag(bootScr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(bootScr, boot_press_cb, LV_EVENT_PRESSED, NULL);
  }
  return true;
}

bool ui_boot_take_touch() {
  const bool t = bootTouched;
  bootTouched = false;
  return t;
}

// The bench page's own "Share SD card" button: the same tap as the WiFi
// screen's (shareTapped -> ui_take_share_tap()), so the loop shares or takes
// the card back in one place. A press on it does not end the page: LVGL
// gives it to the button, not to bootScr's boot_press_cb.
static lv_obj_t *benchShareBtn = NULL, *benchShareLbl = NULL;
static int benchShareState = -1;    // 0 not shared, 1 shared; -1 not drawn yet

void ui_boot_show_bench(const char *text, bool shared) {
  if (!bootScr) return;
  if (!bootBench) {
    // The badge goes up and smaller, the arcs stay as the frame.
    lv_image_set_scale(bootName, 140);                 // 256 = 100 %
    lv_obj_align(bootName, LV_ALIGN_CENTER, 0, -120);
    bootBench = lv_label_create(bootScr);
    lv_obj_set_style_text_color(bootBench, COL_INK, 0);
    lv_obj_set_style_text_font(bootBench, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(bootBench, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(bootBench, 4, 0);
    lv_label_set_long_mode(bootBench, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(bootBench, 300);                  // the chord, with margin
    lv_obj_align(bootBench, LV_ALIGN_CENTER, 0, 30);
    lv_obj_clear_flag(bootBench, LV_OBJ_FLAG_CLICKABLE);

    // Below the text, styled as the WiFi screen's. 48 px tall: a touch
    // just off a 34 px button landed on the page and went to the gauges
    // (2026-10-15). 220 px fits the round panel's chord at its bottom edge
    // (~272 px at 189 below the centre).
    benchShareBtn = lv_button_create(bootScr);
    lv_obj_set_size(benchShareBtn, 220, 48);
    lv_obj_align(benchShareBtn, LV_ALIGN_CENTER, 0, 165);
    lv_obj_set_style_radius(benchShareBtn, 24, 0);
    lv_obj_set_style_bg_color(benchShareBtn, COL_BG, 0);
    lv_obj_set_style_bg_color(benchShareBtn, COL_LINE, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(benchShareBtn, 2, 0);
    lv_obj_set_style_shadow_width(benchShareBtn, 0, 0);
    lv_obj_clear_flag(benchShareBtn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(benchShareBtn, share_btn_cb, LV_EVENT_CLICKED, NULL);
    benchShareLbl = lv_label_create(benchShareBtn);
    lv_obj_set_style_text_font(benchShareLbl, &lv_font_montserrat_14, 0);
    lv_obj_clear_flag(benchShareLbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(benchShareLbl);
  }
  // Compare first: setting the same text still invalidates the label.
  if (strcmp(lv_label_get_text(bootBench), text) != 0) lv_label_set_text(bootBench, text);
  // The button says what a tap does now - and only changes when that does.
  if (benchShareState != (int)shared) {
    benchShareState = shared;
    const lv_color_t c = shared ? COL_OIL : COL_BLUE;
    lv_label_set_text(benchShareLbl, shared ? "Stop sharing SD card" : "Share SD card with PC");
    lv_obj_set_style_text_color(benchShareLbl, c, 0);
    lv_obj_set_style_border_color(benchShareBtn, c, 0);
  }
}

void ui_boot_anim_end() {
  if (!bootScr) return;
  lv_obj_t *old = bootScr;
  bootScr = NULL;                   // clear first: load triggers a redraw
  bootBench = NULL;
  benchShareBtn = benchShareLbl = NULL;   // deleted with the screen
  benchShareState = -1;
  bootTouched = false;
  ui_set_view(ui_current_view());   // back to the remembered screen
  lv_obj_del(old);                  // reclaims the arcs, labels and anims
}

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

#if FRAME_STATS
// Render cost per screen, without anyone swiping: a full-screen redraw, and a
// small 40x40 area like a moving dot leaves behind. Each is timed through
// lv_refr_now(), so it includes the QSPI flush. Run twice - notification rim
// forced on, then off - because the rim sits on the top layer over the whole
// panel and so takes part in every redraw on every screen.
static uint32_t benchRefr(const lv_area_t *a) {
  if (a) lv_obj_invalidate_area(lv_screen_active(), a);
  else   lv_obj_invalidate(lv_screen_active());
  const uint32_t t0 = micros();
  lv_refr_now(NULL);
  return micros() - t0;
}

void ui_bench() {
  const GaugeView was = s_view;
  const bool ringWasHidden = lv_obj_has_flag(noticeRing, LV_OBJ_FLAG_HIDDEN);
  const lv_area_t small = { 300, 260, 339, 299 };   // 40x40, off-centre
  for (int pass = 0; pass < 2; pass++) {
    if (pass == 0) lv_obj_clear_flag(noticeRing, LV_OBJ_FLAG_HIDDEN);
    else           lv_obj_add_flag(noticeRing, LV_OBJ_FLAG_HIDDEN);
    Serial.printf("[BENCH] rim %s\n", pass == 0 ? "ON" : "OFF");
    for (const char *p = settings.views; *p; p++) {
      const GaugeView v = (GaugeView)(*p - '0');
      ui_set_view(v);
      benchRefr(NULL);                                 // settle the load
      uint32_t full = 0, part = 0;
      for (int k = 0; k < 3; k++)  full += benchRefr(NULL);
      for (int k = 0; k < 20; k++) part += benchRefr(&small);
      Serial.printf("[BENCH]   view %d: full %lu us, 40x40 %lu us\n",
                    (int)v, (unsigned long)(full / 3),
                    (unsigned long)(part / 20));
      esp_task_wdt_reset();
    }
  }
  // A real g-meter frame: the dot circling at ~0.8 g, label changing, trail
  // shuffling - each step its own refresh, as at run time.
  if (scr[VIEW_GMETER]) {
    ui_set_view(VIEW_GMETER);
    auto live = [](uint32_t *mx) {
      benchRefr(NULL);
      uint32_t sum = 0; *mx = 0;
      const int N = 60;
      for (int k = 0; k < N; k++) {
        const float a = k * 0.21f;
        ui_update_gmeter(0.8f * cosf(a), 0.8f * sinf(a), 1.0f + 0.2f * sinf(3 * a));
        const uint32_t t0 = micros();
        lv_refr_now(NULL);
        const uint32_t us = micros() - t0;
        sum += us; if (us > *mx) *mx = us;
        delay(16);                     // let the trail timer tick now and then
      }
      esp_task_wdt_reset();
      return sum / N;
    };
    uint32_t mx;
    for (int pass = 0; pass < 2; pass++) {
      if (pass == 0) lv_obj_clear_flag(noticeRing, LV_OBJ_FLAG_HIDDEN);
      else           lv_obj_add_flag(noticeRing, LV_OBJ_FLAG_HIDDEN);
      const uint32_t avg = live(&mx);
      Serial.printf("[BENCH] g-meter live, rim %s: %lu us/frame avg, %lu max\n",
                    pass == 0 ? "ON" : "OFF", (unsigned long)avg,
                    (unsigned long)mx);
    }
    // Cost of one unchanged patch, by size and place: is there a fixed cost
    // per dirty area, and does it depend on what lies under it?
    {
      lv_area_t lab, dot, tiny, blank;
      lv_obj_get_coords(gLabel, &lab);
      lv_obj_get_coords(gDot, &dot);
      tiny  = { 232, 232, 233, 233 };             // 2x2 in the middle
      blank = { 20, 20, 59, 35 };                 // 40x16 corner, off the backdrop
      const lv_area_t *as[] = { &lab, &dot, &tiny, &blank };
      const char *nm[] = { "label", "dot", "2x2", "40x16 corner" };
      for (int i = 0; i < 4; i++) {
        uint32_t sum = 0;
        for (int k = 0; k < 20; k++) sum += benchRefr(as[i]);
        Serial.printf("[BENCH] g-meter live patch %s %dx%d: %lu us\n", nm[i],
                      (int)lv_area_get_width(as[i]), (int)lv_area_get_height(as[i]),
                      (unsigned long)(sum / 20));
      }
      // And an empty refresh: nothing invalidated at all.
      uint32_t sum = 0;
      for (int k = 0; k < 20; k++) {
        const uint32_t t0 = micros(); lv_refr_now(NULL); sum += micros() - t0;
      }
      Serial.printf("[BENCH] g-meter live empty refresh: %lu us\n",
                    (unsigned long)(sum / 20));
    }
    // Rim still off. Same sequence with each object hidden in turn.
    const uint32_t n = lv_obj_get_child_count(scr[VIEW_GMETER]);
    for (uint32_t c = 0; c < n; c++) {
      lv_obj_t *o = lv_obj_get_child(scr[VIEW_GMETER], c);
      lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
      const uint32_t avg = live(&mx);
      lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
      Serial.printf("[BENCH] g-meter live without #%lu (%dx%d): %lu avg, %lu max\n",
                    (unsigned long)c, (int)lv_obj_get_width(o),
                    (int)lv_obj_get_height(o), (unsigned long)avg,
                    (unsigned long)mx);
    }
  }

  // Boost as it changes in the car: a new sample every 2 request slots, a
  // frame every 16 ms in between (so the arc glide is exercised), with small
  // steps per sample (0.01 bar) and big ones (0.06). Rim off, as in the car.
  if (scr[VIEW_BOOST]) {
    ui_set_view(VIEW_BOOST);
    lv_obj_add_flag(noticeRing, LV_OBJ_FLAG_HIDDEN);
    static const float stepBar[] = { 0.01f, 0.06f };
    const uint32_t samplePeriod = BOOST_SAMPLE_MS;
    // Pass 0: everything; 1: number hidden; 2: arc hidden.
    lv_obj_t *const hide[] = { NULL, boostVal, boostArc };
    static const char *const hideName[] = { "", ", number hidden", ", arc hidden" };
    for (int h = 0; h < 3; h++) {
      if (hide[h]) lv_obj_add_flag(hide[h], LV_OBJ_FLAG_HIDDEN);
      for (int s = 0; s < 2; s++) {
        benchRefr(NULL);
        uint32_t sum = 0, mx = 0;
        const int N = 90;                         // ~1.5 s of frames
        const uint32_t start = millis();
        for (int k = 0; k < N; k++) {
          const uint32_t sample = (millis() - start) / samplePeriod;
          ui_update_boost(true, 0.3f + stepBar[s] * sample);
          const uint32_t t0 = micros();
          lv_refr_now(NULL);
          const uint32_t us = micros() - t0;
          sum += us; if (us > mx) mx = us;
          delay(16);
        }
        Serial.printf("[BENCH] boost live, %.2f bar/frame%s: %lu us/frame avg, %lu max\n",
                      stepBar[s], hideName[h], (unsigned long)(sum / N),
                      (unsigned long)mx);
        esp_task_wdt_reset();
      }
      if (hide[h]) lv_obj_clear_flag(hide[h], LV_OBJ_FLAG_HIDDEN);
    }
  }

  // Which objects are expensive: hide each child of the heavier screens in
  // turn (rim still off) and see what a full redraw saves.
  static const GaugeView heavy[] = { VIEW_GMETER, VIEW_OIL, VIEW_BOOST };
  for (unsigned h = 0; h < sizeof(heavy) / sizeof(heavy[0]); h++) {
    if (!scr[heavy[h]]) continue;
    ui_set_view(heavy[h]);
    benchRefr(NULL);
    const uint32_t base = (benchRefr(NULL) + benchRefr(NULL)) / 2;
    Serial.printf("[BENCH] view %d per object (base %lu us):\n",
                  (int)heavy[h], (unsigned long)base);
    const uint32_t n = lv_obj_get_child_count(scr[heavy[h]]);
    for (uint32_t c = 0; c < n; c++) {
      lv_obj_t *o = lv_obj_get_child(scr[heavy[h]], c);
      if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) continue;
      lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
      const uint32_t t = (benchRefr(NULL) + benchRefr(NULL)) / 2;
      lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
      const char *kind = lv_obj_check_type(o, &lv_arc_class)   ? "arc"
                       : lv_obj_check_type(o, &lv_line_class)  ? "line"
                       : lv_obj_check_type(o, &lv_label_class) ? "label"
                       : "obj";
      Serial.printf("[BENCH]   #%lu %s %dx%d: saves %ld us\n", (unsigned long)c, kind,
                    (int)lv_obj_get_width(o), (int)lv_obj_get_height(o),
                    (long)base - (long)t);
      esp_task_wdt_reset();
    }
  }

  if (ringWasHidden) lv_obj_add_flag(noticeRing, LV_OBJ_FLAG_HIDDEN);
  else               lv_obj_clear_flag(noticeRing, LV_OBJ_FLAG_HIDDEN);
  ui_set_view(was);
}
#endif

// --- updates ----------------------------------------------------------------
// Why every updater below starts with a "nothing changed, go home" guard:
//
// LVGL's label and style setters do NOT compare against the current value.
// Each call copies the string and invalidates the object, and loop() runs at
// roughly 200 Hz - so the 48 px oil number was being redrawn ~200 times a
// second. On this partial-render display a label that big spans several flush
// strips, and the result is a visible blink. The coolant number runs identical
// code in a 28 px font, covering fewer strips, so it looked steady: that is
// why oil was reported as flickering and coolant was not. Guarding here also
// takes a large amount of pointless QSPI traffic and CPU off the loop.
void ui_set_car_talking(bool talking) { carTalking = talking; }

void ui_update_oil(bool have, int tempC, bool cold, bool over) {
  static bool lastHave = false, lastCold = false, lastOver = false, lastTalking = false;
  static int  lastTemp = INT32_MIN;
  if (have == lastHave && tempC == lastTemp && cold == lastCold && over == lastOver &&
      carTalking == lastTalking)
    return;
  const bool haveChanged = have != lastHave || lastTemp == INT32_MIN;
  lastHave = have; lastTemp = tempC; lastCold = cold; lastOver = over;
  lastTalking = carTalking;

  if (!have) {
    show_no_value(oilVal);
    // Zero and de-colour the arc too. Leaving it where it froze meant "--"
    // above a full red arc after a dropout at 125 C, which is exactly the
    // "a frozen reading must not look real" rule OBD_STALE_MS exists for.
    lv_arc_set_value(oilArc, 0);
    lv_obj_set_style_arc_color(oilArc, COL_OIL, LV_PART_INDICATOR);
    lv_obj_set_style_image_recolor(oilIcon, COL_DIM, 0);
    // The state line is SHARED with coolant (coolState aliases oilState), so
    // blanking it unconditionally threw away a coolant over-temp warning
    // whenever the oil DID stopped answering - and coolant is the failure that
    // wrecks an engine fastest. Keep the warning if coolant is still over.
    if (over) {
      lv_label_set_text(oilState, "COOLANT TOO HOT");
      lv_obj_set_style_text_color(oilState, COL_BAD, 0);
    } else {
      // Why there is no number, instead of an empty line: the car silent
      // (just woken, ignition off) or talking but this value gone stale.
      lv_label_set_text(oilState, carTalking ? "no oil reading" : "waiting for car");
      lv_obj_set_style_text_color(oilState, COL_DIM, 0);
    }
    return;
  }
  if (haveChanged) show_value_style(oilVal, COL_OIL);
  lv_arc_set_value(oilArc, tempC);
  lv_label_set_text_fmt(oilVal, "%d\xC2\xB0", tempC);
  // The symbol takes the arc's colour: blue cold, oil orange ready, red hot.
  const lv_color_t stateC = over ? COL_BAD : cold ? COL_BLUE : COL_OIL;
  lv_obj_set_style_arc_color(oilArc, stateC, LV_PART_INDICATOR);
  lv_obj_set_style_image_recolor(oilIcon, stateC, 0);
  if (over)      { lv_label_set_text(oilState, "TOO HOT");
                   lv_obj_set_style_text_color(oilState, COL_BAD, 0); }
  else if (cold) { lv_label_set_text(oilState, "COLD - take it easy");
                   lv_obj_set_style_text_color(oilState, COL_BLUE, 0); }
  else           { lv_label_set_text(oilState, "READY");
                   lv_obj_set_style_text_color(oilState, COL_GOOD, 0); }
}

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

void ui_update_coolant(bool have, int tempC, bool over) {
  static bool lastHave = false, lastOver = false;
  static int  lastTemp = INT32_MIN;
  if (have == lastHave && tempC == lastTemp && over == lastOver) return;
  lastHave = have; lastTemp = tempC; lastOver = over;

  if (!have) {
    // Arc and colour too, for the same reason as in ui_update_oil.
    show_no_value(coolVal);
    lv_arc_set_value(coolArc, 0);
    lv_obj_set_style_arc_color(coolArc, COL_BLUE, LV_PART_INDICATOR);
    lv_obj_set_style_image_recolor(coolIcon, COL_DIM, 0);
    return;
  }
  // The colour is set just below; this restores the alignment.
  lv_obj_set_style_text_align(coolVal, LV_TEXT_ALIGN_LEFT, 0);
  lv_arc_set_value(coolArc, tempC);
  lv_label_set_text_fmt(coolVal, "%d\xC2\xB0", tempC);
  // Colour only. The shared state line is owned by ui_update_oil, which is
  // called first each loop and already says TOO HOT when either is over.
  lv_obj_set_style_arc_color(coolArc, over ? COL_BAD : COL_BLUE, LV_PART_INDICATOR);
  lv_obj_set_style_text_color(coolVal, over ? COL_BAD : COL_BLUE, 0);
  lv_obj_set_style_image_recolor(coolIcon, over ? COL_BAD : COL_BLUE, 0);
}

void ui_update_gmeter(float gx, float gy, float gz) {
  // The one updater that was missing the guard described above, so the dot and
  // the "%.2f g" label were still being rewritten at ~200 Hz. Compared at
  // display resolution: pixels for the dot, 2 dp for the label.
  static int lastPx = INT32_MIN, lastPy = INT32_MIN, lastD = INT32_MIN;
  static int lastMagCenti = INT32_MIN;
  // Clamp the VECTOR, not each axis: clamping x and y separately let the dot
  // reach R*sqrt(2) diagonally and sit outside the ring.
  const float R = 205.0f;   // outer ring is 420 px across
  float fx = gx / GMETER_FS_G * R, fy = gy / GMETER_FS_G * R;
  const float len = sqrtf(fx * fx + fy * fy);
  if (len > R) { fx = fx * R / len; fy = fy * R / len; }
  const int px = (int)fx, py = (int)fy;

  // Depth cue: vertical g swells or shrinks the dot, so kerbs and crests read
  // as the point lifting off the sphere. gz sits at ~1 g at rest, so it is the
  // deviation from 1 that matters.
  int d = 26 + (int)((gz - 1.0f) * 12.0f);
  if (d < 14) d = 14;
  if (d > 40) d = 40;
  const float mag = sqrtf(gx * gx + gy * gy);
  const int magCenti = (int)(mag * 100.0f + 0.5f);
  const uint32_t now = lv_tick_get();
  // The dot follows at frame rate, but the number is rate-limited on its own:
  // a figure changing 60 times a second cannot be read, and each rewrite
  // costs ~1.3 ms of a 16 ms frame (measured). Colour goes with the dot, so
  // it still turns red the moment the limit is crossed.
  if (px != lastPx || py != lastPy || d != lastD) {
    lastPx = px; lastPy = py; lastD = d;
    lv_obj_set_size(gDot, d, d);
    lv_obj_align(gDot, LV_ALIGN_CENTER, px, -py);
    lv_obj_set_style_bg_color(gDot, mag > 0.9f ? COL_BAD : COL_GOOD, 0);
  }
  static uint32_t lastLabelMs = 0;
  if (magCenti != lastMagCenti && now - lastLabelMs >= GMETER_LABEL_MS) {
    lastMagCenti = magCenti;
    lastLabelMs = now;
    lv_label_set_text_fmt(gLabel, "%.2f g", mag);
  }

  // Shuffle the trail on its own timer, not every frame: at loop rate the
  // whole history would span a fraction of a second and read as a blur.
  static uint32_t lastTrailMs = 0;
  static int16_t  hx[GMETER_TRAIL_N] = { 0 }, hy[GMETER_TRAIL_N] = { 0 };
  if (now - lastTrailMs < GMETER_TRAIL_MS) return;
  lastTrailMs = now;

  for (int i = GMETER_TRAIL_N - 1; i > 0; i--) { hx[i] = hx[i-1]; hy[i] = hy[i-1]; }
  hx[0] = (int16_t)px; hy[0] = (int16_t)py;
  for (int i = 0; i < GMETER_TRAIL_N; i++)
    lv_obj_align(gTrail[i], LV_ALIGN_CENTER, hx[i], -hy[i]);
}

void ui_update_tpms(const bool have[WHEEL_COUNT], const float bar[WHEEL_COUNT],
                    const int tempC[WHEEL_COUNT]) {
  // Same guard as the other updaters, at display resolution (1 dp of bar).
  static bool lastHave[WHEEL_COUNT] = { false };
  static int  lastDeci[WHEEL_COUNT] = { INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN };
  static int  lastTemp[WHEEL_COUNT] = { INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN };
  bool changed = false;
  for (int i = 0; i < WHEEL_COUNT; i++) {
    const int deci = (int)(bar[i] * 10.0f + 0.5f);
    if (have[i] != lastHave[i] || deci != lastDeci[i] || tempC[i] != lastTemp[i]) {
      changed = true;
      lastHave[i] = have[i]; lastDeci[i] = deci; lastTemp[i] = tempC[i];
    }
  }
  if (!changed) return;

  for (int i = 0; i < WHEEL_COUNT; i++) {
    if (!have[i]) {                       // sensor asleep / not reporting yet
      lv_label_set_text(tyreVal[i], "--");
      lv_obj_set_style_text_color(tyreVal[i], COL_LINE, 0);
      lv_label_set_text(tyreTemp[i], "");
      continue;
    }
    // settings, not the config.h macros. These two were loaded from NVS,
    // rendered into the phone form, clamped and persisted on save - and then
    // read by nothing, so setting a track-day pressure showed "Saved." and
    // changed no behaviour at all. The config.h values are only the defaults
    // that settings_load() falls back to.
    // Per axle: slots are FL, FR, RL, RR, so 0-1 are the fronts.
    const int axle = i < 2 ? 0 : 1;
    const bool bad = bar[i] < settings.tpmsLowBar[axle] ||
                     bar[i] > settings.tpmsHighBar[axle];
    lv_label_set_text_fmt(tyreVal[i], "%.1f", bar[i]);
    // The pressure's own colour is the warning. There used to be a wheel
    // marker coloured green/red as well; the car picture replaced it.
    lv_obj_set_style_text_color(tyreVal[i], bad ? COL_BAD : COL_INK, 0);
    lv_label_set_text_fmt(tyreTemp[i], "%d C", tempC[i]);
  }
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
    lv_label_set_text(gpsSpeed, "--");
    lv_obj_set_style_text_color(gpsSpeed, COL_LINE, 0);
    lv_label_set_text(gpsState, "no module");
    lv_obj_set_style_text_color(gpsState, COL_BAD, 0);
    return;
  }
  if (!haveFix) {                      // talking, but no sky view yet
    lv_label_set_text(gpsSpeed, "--");
    lv_obj_set_style_text_color(gpsSpeed, COL_LINE, 0);
    lv_label_set_text_fmt(gpsState, "searching  %d sats", sats);
    lv_obj_set_style_text_color(gpsState, COL_BLUE, 0);
    return;
  }
  lv_label_set_text_fmt(gpsSpeed, "%d", (int)(speedKmh + 0.5f));
  lv_obj_set_style_text_color(gpsSpeed, COL_GOOD, 0);
  lv_label_set_text_fmt(gpsState, "fix  %d sats", sats);
  lv_obj_set_style_text_color(gpsState, COL_GOOD, 0);
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

  // Every label below has its OWN guard - see the note above ui_update_oil().
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

// Both banners live on lv_layer_top(), so they draw over ANY screen - the boot
// splash included. Suppress them while it is up, or every cold start flashes
// "no CAN link..." across the animation before the bus has had a chance to
// answer. The state is re-applied on the next loop once the splash is gone.
static bool bootSplashUp() { return bootScr != NULL; }

void ui_update_wifi_countdown(float remaining) {
  // A redraw every frame: nothing else moves on this screen, so the sweep
  // can have the frame budget.
  static int last = -2;   // matches neither hidden (-1) nor any step
  int v = -1;
  if (remaining >= 0) {
    v = (int)ceilf(remaining * WIFI_ARC_STEPS);
    if (v > WIFI_ARC_STEPS) v = WIFI_ARC_STEPS;
  }
  if (v == last) return;
  // Show or hide only on a change. Clearing HIDDEN invalidates the whole
  // object even when it is already shown (lv_obj_remove_flag), and doing it
  // on every step redrew the 446 px arc - QR and all - each frame: ~140 ms a
  // frame, the loop down to 7 Hz, and swipes on this screen missed because
  // touch was read that rarely (2026-10-13). lv_arc_set_value() itself only
  // invalidates the sliver that moved.
  if ((v < 0) != (last < 0) || last == -2) {
    if (v < 0) lv_obj_add_flag(wifiArc, LV_OBJ_FLAG_HIDDEN);
    else       lv_obj_clear_flag(wifiArc, LV_OBJ_FLAG_HIDDEN);
  }
  last = v;
  if (v >= 0) lv_arc_set_value(wifiArc, v);
}

void ui_update_usbdisk(bool pcConnected, bool shared) {
  static int last = -1;
  const int s = !pcConnected ? 0 : shared ? 2 : 1;
  if (s == last) return;
  last = s;
  // The button takes the title's place, so only one of the two shows.
  if (s == 0) {
    lv_obj_add_flag(shareBtn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(wifiTitle, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_obj_add_flag(wifiTitle, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(shareBtn, LV_OBJ_FLAG_HIDDEN);
  const lv_color_t c = s == 2 ? COL_OIL : COL_BLUE;
  // While shared, the same button takes the card back (usbdisk_unshare).
  lv_label_set_text(shareLbl, s == 2 ? "Stop sharing SD card" : "Share SD card with PC");
  lv_obj_set_style_text_color(shareLbl, c, 0);
  lv_obj_set_style_border_color(shareBtn, c, 0);
}

bool ui_take_share_tap() {
  const bool t = shareTapped;
  shareTapped = false;
  return t;
}

bool ui_take_wifi_touch() {
  const bool t = wifiTouched;
  wifiTouched = false;
  return t;
}

void ui_update_wifi(const char *ssid, const char *pass, bool apUp,
                    const char *ip, uint8_t clients) {
  // Guarded like the rest. The passphrase changes at most once in a session,
  // but the client count changes whenever a phone comes and goes, so it is
  // part of the comparison - otherwise the count would never refresh.
  static char lastPass[33] = "\x01";     // impossible value, forces first draw
  static char lastSsid[33] = "\x01";
  static bool lastUp = false;
  static int  lastClients = -1;
  const bool passChanged =
      strncmp(lastPass, pass ? pass : "", sizeof(lastPass) - 1) != 0;
  const bool ssidChanged =
      strncmp(lastSsid, ssid ? ssid : "", sizeof(lastSsid) - 1) != 0;
  if (lastUp == apUp && lastClients == (int)clients &&
      !passChanged && !ssidChanged)
    return;
  lastUp = apUp;
  lastClients = (int)clients;
  strncpy(lastPass, pass ? pass : "", sizeof(lastPass) - 1);
  lastPass[sizeof(lastPass) - 1] = '\0';
  strncpy(lastSsid, ssid ? ssid : "", sizeof(lastSsid) - 1);
  lastSsid[sizeof(lastSsid) - 1] = '\0';

  if (clients == 0)      lv_label_set_text(wifiClients, "no phone connected");
  else if (clients == 1) lv_label_set_text(wifiClients, "1 phone connected");
  else                   lv_label_set_text_fmt(wifiClients, "%u connected", clients);
  lv_obj_set_style_text_color(wifiClients, clients ? COL_GOOD : COL_LINE, 0);

  lv_label_set_text(wifiSsid, ssid ? ssid : "");
  lv_label_set_text(wifiPass, pass ? pass : "");
  if (apUp && ip && ip[0]) {
    lv_label_set_text_fmt(wifiIp, "http://%s/", ip);
    lv_obj_set_style_text_color(wifiIp, COL_BLUE, 0);
  } else {
    lv_label_set_text(wifiIp, "starting...");
    lv_obj_set_style_text_color(wifiIp, COL_LINE, 0);
  }

  // Standard WIFI: payload, understood by both Android and iOS cameras.
  // Built here from the live values rather than baked in, because the
  // passphrase is generated per device and changeable from the settings page -
  // a hardcoded code would go stale with nothing to show it had.
  //
  // The separators ; and : and the escape \ are meaningful in this format, so
  // a passphrase containing them would need escaping. Ours cannot: the
  // generator's alphabet is letters and digits only (see WIFI_AP_PASS_ALPHABET),
  // and a passphrase typed on the settings page is the user's own to get wrong.
  // Only when the passphrase actually changed: regenerating a QR is a real
  // amount of work (encode plus a full redraw of a 160 px canvas) and would
  // otherwise run every time a phone connects or drops.
  if (passChanged || ssidChanged) {
    // \ ; , : and " are separators in the WIFI: format and must be escaped, or
    // a name like "Jakub;s car" silently produces a code that either fails to
    // scan or joins the wrong network. This was safe while the SSID was a
    // compile-time constant and the passphrase came from an alphanumeric
    // generator; it stopped being safe the moment both became user-editable
    // from the settings page.
    char payload[160];
    size_t o = 0;
    const char *head = "WIFI:T:WPA;S:";
    while (*head && o < sizeof(payload) - 1) payload[o++] = *head++;
    for (int part = 0; part < 2; part++) {
      const char *s = part == 0 ? (ssid ? ssid : "") : (pass ? pass : "");
      for (; *s && o < sizeof(payload) - 3; s++) {
        if (*s == '\\' || *s == ';' || *s == ',' || *s == ':' || *s == '"')
          payload[o++] = '\\';
        payload[o++] = *s;
      }
      const char *sep = part == 0 ? ";P:" : ";;";
      while (*sep && o < sizeof(payload) - 1) payload[o++] = *sep++;
    }
    payload[o] = '\0';
    lv_qrcode_update(wifiQr, payload, o);
  }
}

void ui_set_faults(uint8_t faults, int oilC, int coolantC) {
  // Suppressed while the boot splash is up, or every cold start would flash a
  // fault across the animation before CAN or the GNSS has had a chance to
  // answer. Folded into the value being compared, so the rim appears on the
  // first call after the splash goes.
  const uint8_t shown = bootSplashUp() ? 0 : faults;
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
  // number - see the note above ui_update_oil().
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
