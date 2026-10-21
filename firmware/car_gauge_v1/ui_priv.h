// ============================================================================
// ui_priv.h  —  what the ui_*.cpp files share, and nothing outside them sees.
// The UI's public face is ui.h. Inside it is one file per screen (ui_temps,
// ui_boost, ui_gmeter, ui_tyres, ui_gps, ui_track, ui_wifi), the notification
// rim (ui_notice), the boot splash and bench page (ui_boot), the widget
// builders every screen uses (ui_widgets) and ui.cpp, which builds them all
// and switches between them.
//
// Why every updater starts with a "nothing changed, go home" guard:
//
// LVGL's label and style setters do NOT compare against the current value.
// Each call copies the string and invalidates the object, and loop() runs at
// roughly 200 Hz - so the 48 px oil number was being redrawn ~200 times a
// second. On this partial-render display a label that big spans several flush
// strips, and the result is a visible blink. The coolant number runs identical
// code in a 28 px font, covering fewer strips, so it looked steady: that is
// why oil was reported as flickering and coolant was not. Guarding here also
// takes a large amount of pointless QSPI traffic and CPU off the loop.
// ============================================================================
#pragma once
#include "ui.h"
#include "config.h"
#include "lvgl.h"

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

#define LCD_PX    466    // the panel, square and round

// The rings every gauge screen is built of, so the screens line up when you
// swipe between them: the outer ring (oil, boost, the splash), the inner one
// inside it (coolant, torque), and the thin one at the very edge (track,
// WiFi). Diameter and line width, in px.
#define RING_OUTER_PX   420
#define RING_OUTER_W    16
#define RING_INNER_PX   372
#define RING_INNER_W    12
#define RING_EDGE_PX    440

// ---- widget builders (ui_widgets.cpp) ---------------------------------------
// A screen: dark, not scrollable, swipes change the view (screen_gesture_cb).
lv_obj_t *make_screen();
// Swipe left / right for the next / previous view. On every screen, and on the
// top layer for a swipe that starts on the notification text.
void screen_gesture_cb(lv_event_t *e);
// For button handlers: was the touch that just ended actually a swipe?
bool touch_was_swipe();
lv_obj_t *make_arc_sized(lv_obj_t *parent, lv_color_t c, int min, int max, int px, int width);
lv_obj_t *make_arc(lv_obj_t *parent, lv_color_t c, int min, int max);
lv_obj_t *make_big_label(lv_obj_t *parent, lv_color_t c, int dy);
lv_obj_t *make_tyre_val(lv_obj_t *parent, int dx, int dy);
lv_obj_t *make_icon_row(lv_obj_t *parent, int dy, const lv_image_dsc_t *icon, lv_obj_t **iconOut);
void fix_value_width(lv_obj_t *label, const char *pattern);
void show_no_value(lv_obj_t *label);
void show_value_style(lv_obj_t *label, lv_color_t c);
// For updates: set a colour only when it differs. Any style set invalidates
// the whole object, unchanged value or not - an arc's whole bounding box.
void set_arc_color(lv_obj_t *arc, lv_color_t c);
void set_text_color(lv_obj_t *label, lv_color_t c);
void set_recolor(lv_obj_t *img, lv_color_t c);
lv_obj_t *make_name(lv_obj_t *parent, const char *txt, int dy);

// ---- the screens: each builds and returns its own ---------------------------
lv_obj_t *ui_build_temps();    // ui_temps.cpp:  oil and coolant
lv_obj_t *ui_build_boost();    // ui_boost.cpp:  boost, torque, pedal
lv_obj_t *ui_build_gmeter();   // ui_gmeter.cpp
lv_obj_t *ui_build_tyres();    // ui_tyres.cpp
lv_obj_t *ui_build_gps();      // ui_gps.cpp
lv_obj_t *ui_build_track();    // ui_track.cpp:  lap timer
lv_obj_t *ui_build_wifi();     // ui_wifi.cpp
void ui_build_notice();        // ui_notice.cpp: the rim, on lv_layer_top

// ---- across files ------------------------------------------------------------
// The WiFi screen's and the bench page's "Share SD card" buttons are the same
// tap (ui_wifi.cpp; read with ui_take_share_tap()).
void ui_share_btn_cb(lv_event_t *e);
// ui_boot_splash_up() (ui.h): the rim stays hidden while the splash is up.

#if BENCH_CONSOLE
// For ui_bench() (ui_bench.cpp), which times screens from the inside.
lv_obj_t *ui_screen(GaugeView v);   // ui.cpp; NULL for one not built
lv_obj_t *ui_notice_ring();         // ui_notice.cpp
lv_obj_t *ui_gmeter_label();        // ui_gmeter.cpp
lv_obj_t *ui_gmeter_dot();
lv_obj_t *ui_boost_value();         // ui_boost.cpp
lv_obj_t *ui_boost_arc();
#endif
