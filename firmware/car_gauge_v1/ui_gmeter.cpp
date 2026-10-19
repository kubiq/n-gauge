// ============================================================================
// ui_gmeter.cpp  —  the g-meter: a faked sphere with a g-g trail across it.
// See ui_priv.h.
// ============================================================================
#include "ui_priv.h"
#include <math.h>            // sqrtf, cosf, sinf
#include <esp_heap_caps.h>   // the g-meter backdrop's canvas goes in PSRAM

// gmeter widgets. gTrail[0] is the newest history dot, [N-1] the oldest.
static lv_obj_t *gDot, *gLabel;
static lv_obj_t *gTrail[GMETER_TRAIL_N];

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

// G-METER: a faked sphere with a g-g trail across it.
lv_obj_t *ui_build_gmeter() {
  lv_obj_t *s = make_screen();
  make_gmeter_backdrop(s);

  // Trail: oldest drawn first so the newest sits on top. Older = smaller and
  // more transparent, which is what makes the direction of travel readable.
  for (int i = GMETER_TRAIL_N - 1; i >= 0; i--) {
    const float age = (float)i / GMETER_TRAIL_N;         // 0 = newest
    gTrail[i] = make_dot(s, 8 + (int)(8 * (1.0f - age)),
                         COL_BLUE, (lv_opa_t)(40 + 160 * (1.0f - age)));
  }
  gDot   = make_dot(s, 26, COL_GOOD, LV_OPA_COVER);
  gLabel = make_name(s, "G-FORCE", 150);
  return s;
}

#if BENCH_CONSOLE
lv_obj_t *ui_gmeter_label() { return gLabel; }
lv_obj_t *ui_gmeter_dot()   { return gDot; }
#endif

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
