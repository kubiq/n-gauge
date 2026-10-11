// ============================================================================
// settings.h  —  user-tunable values, persisted in NVS.
//   The config.h macros are the DEFAULTS; these are the live values. Anything
//   editable from the phone must live here, or it resets every ignition cycle.
// ============================================================================
#pragma once
#include <stddef.h>
#include <stdint.h>

struct Settings {
  uint8_t brightness;     // 0-255, AMOLED - by day
  bool    nightOn;        // dim to nightBright once the sun is down
  uint8_t nightBright;    // 10-255
  uint8_t motionWakeMg;   // wake-on-motion threshold, mg; 0 = off
  uint8_t sleepAfterMin;  // sleep this long after the car goes quiet, 1-60
  uint8_t logLevel;       // LogLevel (logger.h): 0 off .. 3 full
  int16_t oilReadyC;      // below this = "cold, take it easy"
  int16_t oilWarnC;       // at/above = over-temp alarm
  int16_t coolantWarnC;
  float   tpmsLowBar[2];      // red-tyre limits per axle: [0] front, [1] rear
  float   tpmsHighBar[2];
  uint16_t tripGapMin;    // minutes without an accepted GPS point before the
                          // trip distance restarts, 1-720
  int8_t  tzOffset;       // whole hours from UTC, -12..+14
  bool    tzDst;          // also apply the EU summer-time rule
  char    tz[40];         // DERIVED: POSIX TZ rule built from the two above.
                          // Never edit directly - settings_set_tz_offset owns it.
  uint8_t lastView;       // GaugeView that was on screen when we last saved
  // G-meter zero. The pod is mounted tilted on the dash, so gravity leaks
  // into the lateral/longitudinal axes and the dot sits off-centre at rest.
  // These are the at-rest readings, subtracted before display. Persisted
  // because the mounting angle is fixed - it wants calibrating once, not
  // re-guessing every boot.
  // The full at-rest vector, not just two axes: which board axis points down
  // depends on how the pod is mounted, and the g-meter has to know that to
  // tell "vertical" from "lateral". Measured -0.925 on X in the first pod,
  // i.e. X was the vertical axis, not Z as the original code assumed.
  float   gmOffX;
  float   gmOffY;
  float   gmOffZ;
  bool    gmCal;          // false = never calibrated, so auto-zero when still
  char    apSsid[33];     // 1-32 chars. Empty would make softAP fail.
  char    apPass[33];     // WPA2: 8-32 chars. Shorter would make softAP fail
                          // and lock you out, so it is validated on write.
  // The phone's public key the logs are encrypted to (logcrypt.h, DECISIONS
  // "Logs are to be encrypted to a public key"). Public: nothing here can
  // decrypt. No key, no encryption - logKey false means plaintext logs.
  bool    logKey;
  uint8_t logPub[32];
};

// The log encryption key, as the phone sends it: 64 hex digits, or "off" to
// stop encrypting. False (nothing changed) for anything else.
bool settings_set_log_key(const char *hex);
// Changes whenever the key does (not persisted): the logger closes its files
// on a change, so the next ones use the new key.
extern volatile uint32_t settings_log_key_gen;

// Returns false (and changes nothing) if pass is not a valid WPA2 length.
bool settings_set_ap_pass(const char *pass);
// Returns false (and changes nothing) if ssid is empty or over 32 bytes.
bool settings_set_ap_ssid(const char *ssid);

// Set the clock offset from UTC in whole hours, optionally with the EU
// summer-time rule. Builds the POSIX TZ string, persists, and applies it to
// libc so localtime_r() honours it immediately. False if hours is out of range.
bool settings_set_tz_offset(int8_t hours, bool dst);
void settings_apply_tz();   // push settings.tz into libc

// Persist just the current screen. Separate from settings_save() because it
// happens on a different cadence - whenever the driver changes view - and
// there is no reason to rewrite every other key for it.
void settings_save_view(uint8_t view);

// Store the current at-rest accelerometer reading as the g-meter zero.
void settings_set_gmeter_zero(float x, float y, float z);

extern Settings settings;

// ---- the plain settings, as one table ----------------------------------------
// Every simple setting - a number or a checkbox with a default and a range -
// is one row of settings_table (settings.cpp): its NVS key, which is also its
// field name on the settings pages' forms, its name in /api/settings, its
// type and limits. Loading, saving, the /save handler and the JSON all walk
// the table, so a new setting is a field above, a row there and an input on
// a page (2026-10-09; before, it was five hand-kept lists).
// Not in the table: the WiFi name and password, the time zone, the g-meter
// zero and the last view - each has its own setter with more to it.
enum SettingType : uint8_t { SET_U8, SET_U16, SET_I16, SET_F32, SET_BOOL };
struct SettingDef {
  const char *key;      // NVS key = the form field's name
  const char *json;     // name in /api/settings
  SettingType type;
  void       *field;    // where in `settings`
  float       def, lo, hi;
};
extern const SettingDef settings_table[];
extern const size_t     settings_table_n;
float setting_get(const SettingDef &d);
// Clamped to [lo, hi]; NaN is ignored. settings_save() then applies the rules
// between settings (see settings_fixup in settings.cpp) before writing.
void  setting_set(const SettingDef &d, float v);

// ---- sleep history in NVS --------------------------------------------------
// A ring of the last SLEEP_LOG_NVS_N sleep decisions. Exists because the two
// existing channels both vanish exactly when the parked behaviour needs
// explaining: /sleeplog.txt needs an SD card, and serial is dead across deep
// sleep - and a board that wakes and sleeps in under two seconds cannot be
// read off the screen either.
//
// awakeMs is millis() at the moment of sleeping, i.e. how long that wake cost.
// wakeCause is esp_sleep_get_wakeup_cause(): 0 cold boot, 3 EXT1, 4 timer.
struct SleepRec {
  uint32_t awakeMs;
  uint32_t mask;          // ext1 pins armed; bit 0 button, bit 16 CAN RX
  uint16_t fruitless;
  uint8_t  wakeCause;
  uint8_t  canLevel;
};

void settings_log_sleep(const SleepRec *r);   // append to the ring
void settings_dump_sleep_log();               // print it, oldest first

void settings_load();   // NVS -> settings, falling back to config.h defaults
void settings_save();   // settings -> NVS
