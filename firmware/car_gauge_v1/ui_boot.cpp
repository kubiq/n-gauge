// ============================================================================
// ui_boot.cpp  —  the boot splash, the bench page it turns into on USB power,
// and the one-off notice. See ui_priv.h.
// ============================================================================
#include "ui_priv.h"
#include <string.h>     // strcmp in the bench text guard

// Hyundai N badge for the boot splash (n_logo_img.c), full colour: the
// badge's near-black outline and shadow are lifted to grey in the source PNG
// (firmware/assets/hyundai_n_logo.png), or they vanish on the dark panel.
extern "C" const lv_image_dsc_t n_logo;

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
  bootOuter = make_arc_sized(bootScr, COL_OIL,  0, 100, RING_OUTER_PX, RING_OUTER_W);
  bootInner = make_arc_sized(bootScr, COL_BLUE, 0, 100, RING_INNER_PX, RING_INNER_W);
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
// screen's (ui_share_btn_cb -> ui_take_share_tap()), so the loop shares or
// takes the card back in one place. A press on it does not end the page:
// LVGL gives it to the button, not to bootScr's boot_press_cb.
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
    lv_obj_add_event_cb(benchShareBtn, ui_share_btn_cb, LV_EVENT_CLICKED, NULL);
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

// The notification rim lives on lv_layer_top(), so it draws over ANY screen -
// the boot splash included. ui_set_faults() suppresses it while the splash is
// up, or every cold start flashes "no CAN link..." across the animation before
// the bus has had a chance to answer. The state is re-applied on the next loop
// once the splash is gone.
bool ui_boot_splash_up() { return bootScr != NULL; }
