// ============================================================================
// ui.h  —  LVGL gauge UI (real LVGL code; version ~9.x).
// ============================================================================
#pragma once
#include <stdbool.h>
#include <stdint.h>

// All views that exist. Which of them are actually reachable, and in what
// order, is settings.views (set from the app; default VIEW_ORDER_DEFAULT).
// VIEW_OIL shows oil AND coolant together (concentric arcs). VIEW_COOLANT is
// retained only so the stored "last view" numbering doesn't shift; no screen
// is built for it and ui_set_view rejects it.
// VIEW_WIFI is appended rather than inserted, so a lastView already stored in
// NVS keeps meaning the same screen.
enum GaugeView { VIEW_OIL = 0, VIEW_COOLANT, VIEW_BOOST, VIEW_GMETER, VIEW_TPMS,
                 VIEW_GPS, VIEW_WIFI, VIEW_TRACK, VIEW_COUNT };
// VIEW_TRACK appended for the same reason as VIEW_WIFI: lastView is stored in
// NVS as a number, so inserting would silently change what old values mean.

// Wheel order matches the TPMS payload's four-byte-per-wheel slots (pressure at
// 4 + i*4, temperature at 5 + i*4). Front vs rear is confirmed; left vs right
// within an axle is still assumed.
enum Wheel { WHEEL_FL = 0, WHEEL_FR, WHEEL_RL, WHEEL_RR, WHEEL_COUNT };

void ui_init();                       // build all screens (call after LVGL is up)
void ui_set_view(GaugeView v);        // switch active screen
// Step through the screens: +1 = next (swipe left), -1 = previous (swipe right).
void ui_step_view(int delta);
GaugeView ui_current_view();
// Is the view in the cycle (settings.views), i.e. not hidden?
bool ui_view_shown(GaugeView v);
// Where to land when the remembered screen will not do (WiFi, or hidden):
// the first view of the cycle other than WiFi.
GaugeView ui_home_view();
// FRAME_STATS only: time a full and a small redraw on every screen, rim on
// and off, and print the results. Blocks for a few seconds.
void ui_bench();

// Feed data in; the UI decides colors/labels/states.
// Whether the car is answering at all (a non-TPMS frame within OBD_STALE_MS).
// Only says why a value is missing: "waiting for car" vs "no oil reading".
void ui_set_car_talking(bool talking);
void ui_update_oil(bool have, int tempC, bool cold, bool over);
void ui_update_coolant(bool have, int tempC, bool over);
void ui_update_boost(bool have, float bar);
// Engine torque ESTIMATE in Nm (TORQUE_NM_FROM_LOAD), inside the boost arc.
void ui_update_torque(bool have, float nm);
// Accelerator pedal, 0-100 % (foot off .. floored), as a bar under the boost.
void ui_update_pedal(bool have, int pct);
// gz drives the depth cue (dot size) on the sphere. Call at your loop rate;
// the trail samples itself on its own timer.
void ui_update_gmeter(float gx, float gy, float gz);
// One entry per Wheel. have[i]=false means that sensor hasn't reported yet
// (TPMS sensors sleep while parked), shown as "--" rather than 0.0.
void ui_update_tpms(const bool have[WHEEL_COUNT], const float bar[WHEEL_COUNT],
                    const int tempC[WHEEL_COUNT]);
// linkUp = NMEA arriving at all (module wired, baud right); haveFix = usable
// position. Shown separately so "no module" can't be mistaken for "no signal".
// tripKm comes from logger_trip_km(), not from gps.cpp: it is summed only over
// fixes that passed every GPX gate, so a tunnel spike cannot add distance.
void ui_update_gps(bool linkUp, bool haveFix, int sats, float speedKmh,
                   float tripKm);
// Lap timer screen. Takes the state from laps_info(); its START and STOP
// buttons call laps_start() / laps_stop() directly.
struct LapInfo;
void ui_update_track(const LapInfo *info);

// Faults for the notification area: a coloured rim around the whole panel plus
// one line of text, over whichever screen is loaded.
//
// The bit ORDER is the priority. The text shows one fault at a time, starting
// with the highest-priority one, with "i/n" when there are several - tap it to
// step through the rest. So add new faults in the position
// that reflects how much they matter, and add their text to the table in
// ui_set_faults() at the same index. CAN is red; the others are amber, because
// the gauge still works without them.
//
// "No GPS fix" is deliberately NOT a fault. It is normal in a tunnel or a
// garage and would flash constantly; the GPS screen already shows it.
enum UiFault : uint8_t {
  FAULT_CAN = 1u << 0,   // no CAN traffic: the gauge cannot do its job  (red)
  FAULT_GPS = 1u << 1,   // GNSS module not talking at all               (amber)
  FAULT_SD  = 1u << 2,   // no SD card: nothing is being logged          (amber)
};

// Show the given faults (a mask of UiFault). Safe to call every loop: it
// compares before touching anything, which for a full-panel shape is essential,
// not tidiness.
void ui_set_faults(uint8_t faults);
// Shows the AP SSID + IP on screen while the settings AP is up, so you know
// what to connect to without needing a serial console.
// The WiFi screen: network name, passphrase and IP, so they can be read from
// the driver's seat. This is the only route to the settings panel now that the
// pod's buttons are unreachable and the passphrase is randomly generated per
// device rather than printed in the source.
// The "Share SD card with PC" button on the WiFi screen (usbdisk.cpp): shown
// only with a PC connected; while shared it reads as a status instead.
void ui_update_usbdisk(bool pcConnected, bool shared);
// The WiFi screen's countdown arc: remaining fraction 1..0 until it returns
// to the gauges by itself; negative hides it.
void ui_update_wifi_countdown(float remaining);
bool ui_take_share_tap();      // true once per tap on it
bool ui_take_wifi_touch();     // true once per press on the WiFi screen
void ui_update_wifi(const char *ssid, const char *pass, bool apUp,
                    const char *ip, uint8_t clients);

// Boot splash: the gauge arcs sweep up and the name fades in, then it hands
// over to the remembered screen. Non-blocking on purpose - start it, keep
// running the loop, and call ui_boot_anim_end() once ui_boot_anim_done() is
// true. Blocking through it would stall the GNSS UART and risk the watchdog.
void ui_boot_anim_start();

// Put a progress line on the splash and draw it immediately. Safe to call
// before loop() exists, and a no-op once the splash has been handed over.
//
// This is the only way to see what setup() is doing on buck power, where there
// is no serial console - and where the board may sleep from inside
// power_boot_check() without loop() ever running.
// Put a message in the middle of the panel, over whatever is showing, and draw
// it immediately. For things that happen where no log can be read: the sleep
// decision is written to /sleeplog.txt and to serial, and on buck power there
// is no serial and often no card - so "why did it just go black" had no
// answer at all. Shows over the splash or a gauge screen equally.
void ui_show_notice(const char *msg);   // NULL hides it
// True once the sweep has run. The loop then HOLDS the splash - the N badge
// rather than gauges reading "--" - until the car sends a reading, or the
// "no car" fault would show anyway; ui_boot_anim_end() hands over.
bool ui_boot_anim_done();
// A press on the held splash, once: the driver wants the gauges now.
bool ui_boot_take_touch();
// On the bench (a PC on USB) the held splash becomes a status page: the
// badge moves up, `text` goes under it, and a "Share SD card" button below
// (its tap is ui_take_share_tap()'s, as the WiFi screen's; `shared` says
// whether it reads "Stop sharing" instead). Call again to refresh.
void ui_boot_show_bench(const char *text, bool shared);
void ui_boot_anim_end();
