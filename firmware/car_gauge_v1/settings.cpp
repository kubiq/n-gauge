#include "settings.h"
#include "config.h"
#include "calc.h"
#include "ui.h"             // VIEW_WIFI: never persisted, see settings_save_view
#include <Preferences.h>
#include <esp_random.h>
#include <math.h>           // lroundf, isnan for the table's values
#include <string.h>
#include <stdlib.h>
#include <time.h>

Settings settings;
static Preferences prefs;

#define NVS_NAMESPACE "gauge"

// The POSIX TZ string is built by calc_build_tz() in calc.h, which the host
// tests in tests/ exercise directly - the sign inversion it has to get right
// is the kind of thing that is easy to reverse and invisible until a log is
// stamped an hour out. The DST rule there is the EU one (last Sunday in March
// to last Sunday in October), correct for Europe and wrong elsewhere, which
// is why the settings page labels the checkbox as such.
static void buildTz() {
  calc_build_tz(settings.tz, sizeof(settings.tz), settings.tzOffset,
                settings.tzDst);
}

// Generate a per-device AP passphrase.
//
// esp_random() is the hardware RNG and is properly seeded once WiFi or
// Bluetooth has been active; at first boot before radio init it falls back to
// a less strong source, which is why this is called from settings_load()
// during setup rather than depending on the radio being up. Even the weak case
// is vastly better than a passphrase published in the repository.
static void genApPass(char *out, size_t n) {
  static const char alphabet[] = WIFI_AP_PASS_ALPHABET;
  const size_t alen = sizeof(alphabet) - 1;       // minus the NUL
  size_t len = WIFI_AP_PASS_LEN;
  if (len > n - 1) len = n - 1;
  for (size_t i = 0; i < len; i++) out[i] = alphabet[esp_random() % alen];
  out[len] = '\0';
}

// ---- the plain settings ------------------------------------------------------
// One row per setting - see settings.h. The keys are the NVS keys the
// firmware has always used, with the same stored types, so existing values
// load unchanged. A stored value outside [lo, hi] (or NaN) loads as the
// default: a brightness of 0 would leave a black screen, and the phone page
// is the only way back. From the phone, values are clamped into range.
const SettingDef settings_table[] = {
  // key        json             type      field                     default                     lo     hi
  { "bright",   "brightness",    SET_U8,   &settings.brightness,     LCD_BRIGHTNESS_DEFAULT,     10,    255 },
  { "nightOn",  "nightOn",       SET_BOOL, &settings.nightOn,        NIGHT_ON_DEFAULT,           0,     1 },
  { "nightBr",  "nightBright",   SET_U8,   &settings.nightBright,    NIGHT_BRIGHTNESS_DEFAULT,   10,    255 },
  // 0 = off; 1-29 becomes 30 in settings_fixup().
  { "womMg",    "motionWakeMg",  SET_U8,   &settings.motionWakeMg,   MOTION_WAKE_MG_DEFAULT,     0,     255 },
  { "sleepMin", "sleepAfterMin", SET_U8,   &settings.sleepAfterMin,  BUS_IDLE_SLEEP_MS / 60000,  1,     60 },
  { "logLvl",   "logLevel",      SET_U8,   &settings.logLevel,       LOG_LEVEL_DEFAULT,          0,     3 },
  // A warning at 0 would leave the gauge permanently in alarm, which trains
  // the driver to ignore it - worse than no warning at all.
  { "oilRdy",   "oilReadyC",     SET_I16,  &settings.oilReadyC,      OIL_READY_C,                40,    150 },
  { "oilWarn",  "oilWarnC",      SET_I16,  &settings.oilWarnC,       OIL_WARN_C,                 90,    200 },
  { "cool",     "coolantWarnC",  SET_I16,  &settings.coolantWarnC,   COOLANT_WARN_C,             90,    200 },
  // Per axle since 2026-10-04, under new keys: the old single pair ("tLow",
  // "tHigh") is ignored, so the manual's per-axle defaults apply until edited.
  { "tLowF",    "tpmsLowBarFront",  SET_F32, &settings.tpmsLowBar[0],  TPMS_LOW_BAR_FRONT,     0.5f,  5.0f },
  { "tHighF",   "tpmsHighBarFront", SET_F32, &settings.tpmsHighBar[0], TPMS_HIGH_BAR_FRONT,    0.5f,  5.0f },
  { "tLowR",    "tpmsLowBarRear",   SET_F32, &settings.tpmsLowBar[1],  TPMS_LOW_BAR_REAR,      0.5f,  5.0f },
  { "tHighR",   "tpmsHighBarRear",  SET_F32, &settings.tpmsHighBar[1], TPMS_HIGH_BAR_REAR,     0.5f,  5.0f },
  // 0 would restart the trip on every point, i.e. always read zero; 720
  // minutes is half a day, far past any plausible stop.
  { "tripGap",  "tripGapMin",    SET_U16,  &settings.tripGapMin,     TRIP_GAP_MIN,               1,     720 },
};
const size_t settings_table_n = sizeof(settings_table) / sizeof(settings_table[0]);

float setting_get(const SettingDef &d) {
  switch (d.type) {
    case SET_U8:   return *(uint8_t  *)d.field;
    case SET_U16:  return *(uint16_t *)d.field;
    case SET_I16:  return *(int16_t  *)d.field;
    case SET_F32:  return *(float    *)d.field;
    case SET_BOOL: return *(bool     *)d.field ? 1 : 0;
  }
  return 0;
}

// Stores v as is - callers decide what to do with an out-of-range value.
static void storeRaw(const SettingDef &d, float v) {
  switch (d.type) {
    case SET_U8:   *(uint8_t  *)d.field = (uint8_t)lroundf(v);  break;
    case SET_U16:  *(uint16_t *)d.field = (uint16_t)lroundf(v); break;
    case SET_I16:  *(int16_t  *)d.field = (int16_t)lroundf(v);  break;
    case SET_F32:  *(float    *)d.field = v;                    break;
    case SET_BOOL: *(bool     *)d.field = v != 0;               break;
  }
}

void setting_set(const SettingDef &d, float v) {
  // NaN first: every comparison against it is false, so the clamps below
  // would let it through - and a NaN tyre threshold makes `bar < low` false
  // for every wheel, forever. toFloat() (atof) parses "nan" happily.
  if (isnan(v)) return;
  storeRaw(d, v < d.lo ? d.lo : (v > d.hi ? d.hi : v));
}

// Rules BETWEEN settings, which no single row's range can express. Run after
// loading and before every save.
static void settings_fixup() {
  // Below 30 mg a desk or a parked car is never still (50 mg fired on
  // keyboard typing), so 1-29 is taken as a slip; 0 stays "off".
  if (settings.motionWakeMg != 0 && settings.motionWakeMg < 30) settings.motionWakeMg = 30;
  // Thresholds the wrong way round would make every tyre red, or the oil
  // "warmed up" only above its alarm: back to the defaults, as a pair.
  static const float lowDef[2]  = { TPMS_LOW_BAR_FRONT,  TPMS_LOW_BAR_REAR };
  static const float highDef[2] = { TPMS_HIGH_BAR_FRONT, TPMS_HIGH_BAR_REAR };
  for (int a = 0; a < 2; a++) {
    if (settings.tpmsLowBar[a] >= settings.tpmsHighBar[a]) {
      settings.tpmsLowBar[a]  = lowDef[a];
      settings.tpmsHighBar[a] = highDef[a];
    }
  }
  if (settings.oilWarnC <= settings.oilReadyC) {
    settings.oilReadyC = OIL_READY_C;
    settings.oilWarnC  = OIL_WARN_C;
  }
}

void settings_load() {
  prefs.begin(NVS_NAMESPACE, /*readOnly=*/true);
  // Each getX() falls back to the config.h default when the key is absent,
  // so a fresh board and a wiped NVS both behave like the compiled defaults.
  for (size_t i = 0; i < settings_table_n; i++) {
    const SettingDef &d = settings_table[i];
    float v;
    switch (d.type) {
      case SET_U8:   v = prefs.getUChar(d.key,  (uint8_t)d.def);  break;
      case SET_U16:  v = prefs.getUShort(d.key, (uint16_t)d.def); break;
      case SET_I16:  v = prefs.getShort(d.key,  (int16_t)d.def);  break;
      case SET_F32:  v = prefs.getFloat(d.key,  d.def);           break;
      default:       v = prefs.getBool(d.key,   d.def != 0) ? 1 : 0; break;
    }
    // Written as `!(in range)` on purpose, so NaN is rejected too.
    storeRaw(d, (v >= d.lo && v <= d.hi) ? v : d.def);
  }
  // Not trimmed here: settings_set_ap_ssid() trims on the way in, so a stored
  // name cannot have stray whitespace to begin with.
  String ssid = prefs.getString("apSsid", WIFI_AP_SSID);
  strncpy(settings.apSsid, ssid.c_str(), sizeof(settings.apSsid) - 1);
  settings.apSsid[sizeof(settings.apSsid) - 1] = '\0';
  // An empty SSID makes softAP() fail, which would take the only way in with
  // it - the pod's buttons are unreachable once mounted.
  if (settings.apSsid[0] == '\0') {
    strncpy(settings.apSsid, WIFI_AP_SSID, sizeof(settings.apSsid) - 1);
    settings.apSsid[sizeof(settings.apSsid) - 1] = '\0';
  }

  String pass = prefs.getString("apPass", "");
  strncpy(settings.apPass, pass.c_str(), sizeof(settings.apPass) - 1);
  settings.apPass[sizeof(settings.apPass) - 1] = '\0';
  // No stored passphrase (first boot or wiped NVS), or one too short for WPA2
  // - which would make softAP() fail and lock us out entirely. Generate a
  // random per-device one rather than falling back to a compiled-in default:
  // there isn't one any more, on purpose. Written back below so it survives
  // the next boot, and shown on the WiFi screen so it can be read in the car.
  bool freshPass = false;
  if (strlen(settings.apPass) < 8) {
    genApPass(settings.apPass, sizeof(settings.apPass));
    freshPass = true;
  }
  settings.tzOffset = (int8_t)prefs.getChar("tzOff", TZ_OFFSET_DEFAULT);
  settings.tzDst    = prefs.getBool("tzDst", TZ_DST_DEFAULT);
  if (settings.tzOffset < -12 || settings.tzOffset > 14)
    settings.tzOffset = TZ_OFFSET_DEFAULT;
  buildTz();
  settings.lastView = prefs.getUChar("view", 0);
  settings.gmOffX   = prefs.getFloat("gmOffX", 0.0f);
  settings.gmOffY   = prefs.getFloat("gmOffY", 0.0f);
  settings.gmOffZ   = prefs.getFloat("gmOffZ", 0.0f);
  // Key is versioned: an earlier build stored only X and Y, so a calibration
  // written by it has gmOffZ silently 0 and would mis-identify the vertical
  // axis. Reading a new key makes those stale entries fail closed into a
  // fresh auto-zero instead of being trusted.
  settings.gmCal    = prefs.getBool("gmCal3", false);
  // A 32-byte blob or nothing: anything else stored there is not a key.
  settings.logKey = prefs.isKey("logPub") &&
                    prefs.getBytes("logPub", settings.logPub, sizeof(settings.logPub)) == sizeof(settings.logPub);
  if (!settings.logKey) memset(settings.logPub, 0, sizeof(settings.logPub));
  prefs.end();
  settings_fixup();

  // Persist a freshly generated passphrase. Done after prefs.end() because the
  // handle above is read-only; settings_save() opens its own writable one.
  // Without this the AP would present a different passphrase on every boot.
  if (freshPass) {
    settings_save();
    Serial.printf("[OK] generated AP passphrase (see the WiFi screen)\n");
  }

  settings_apply_tz();
}

void settings_save() {
  settings_fixup();
  prefs.begin(NVS_NAMESPACE, /*readOnly=*/false);
  for (size_t i = 0; i < settings_table_n; i++) {
    const SettingDef &d = settings_table[i];
    const float v = setting_get(d);
    switch (d.type) {
      case SET_U8:   prefs.putUChar(d.key,  (uint8_t)v);  break;
      case SET_U16:  prefs.putUShort(d.key, (uint16_t)v); break;
      case SET_I16:  prefs.putShort(d.key,  (int16_t)v);  break;
      case SET_F32:  prefs.putFloat(d.key,  v);           break;
      case SET_BOOL: prefs.putBool(d.key,   v != 0);      break;
    }
  }
  prefs.putString("apSsid", settings.apSsid);
  prefs.putString("apPass", settings.apPass);
  prefs.putChar("tzOff",   settings.tzOffset);
  prefs.putBool("tzDst",   settings.tzDst);
  prefs.putUChar("view",   settings.lastView);
  prefs.putFloat("gmOffX", settings.gmOffX);
  prefs.putFloat("gmOffY", settings.gmOffY);
  prefs.putFloat("gmOffZ", settings.gmOffZ);
  prefs.putBool("gmCal3",  settings.gmCal);
  if (settings.logKey) prefs.putBytes("logPub", settings.logPub, sizeof(settings.logPub));
  else                 prefs.remove("logPub");
  prefs.end();
}

volatile uint32_t settings_log_key_gen = 0;

bool settings_set_log_key(const char *hex) {
  if (!hex) return false;
  if (strcmp(hex, "off") == 0) {
    if (!settings.logKey) return true;
    settings.logKey = false;
    memset(settings.logPub, 0, sizeof(settings.logPub));
  } else {
    uint8_t pub[32];
    if (!calc_parse_hex(hex, pub, sizeof(pub))) return false;
    if (settings.logKey && memcmp(pub, settings.logPub, sizeof(pub)) == 0) return true;
    memcpy(settings.logPub, pub, sizeof(pub));
    settings.logKey = true;
  }
  settings_save();
  settings_log_key_gen++;
  return true;
}

void settings_set_gmeter_zero(float x, float y, float z) {
  settings.gmOffX = x;
  settings.gmOffY = y;
  settings.gmOffZ = z;
  settings.gmCal  = true;
  settings_save();
}

void settings_save_view(uint8_t view) {
  // Refuse VIEW_WIFI here, where it covers every caller. The pod boots onto
  // the stored view, so persisting this one means powering up with the AP
  // passphrase and a join-the-network QR code on display - readable, or
  // photographable, through the windscreen of a parked car. The check used to
  // live at the call site in the sketch, which protected that one path only.
  if (view == VIEW_WIFI) return;
  settings.lastView = view;
  prefs.begin(NVS_NAMESPACE, /*readOnly=*/false);
  prefs.putUChar("view", view);
  prefs.end();
}

bool settings_set_ap_ssid(const char *ssid) {
  if (!ssid) return false;
  // Trimmed on the way in - a phone keyboard appends a space and a trailing
  // space in an SSID is legal but invisible. Rejected rather than truncated if
  // it is too long, so a name silently losing its tail is impossible.
  if (strlen(ssid) > sizeof(settings.apSsid) - 1) return false;
  char clean[sizeof(settings.apSsid)];
  if (calc_trim_copy(clean, sizeof(clean), ssid) == 0) return false;
  strncpy(settings.apSsid, clean, sizeof(settings.apSsid) - 1);
  settings.apSsid[sizeof(settings.apSsid) - 1] = '\0';
  settings_save();
  return true;
}

bool settings_set_ap_pass(const char *pass) {
  if (!pass) return false;
  const size_t n = strlen(pass);
  if (n < 8 || n > sizeof(settings.apPass) - 1) return false;   // WPA2 minimum
  strncpy(settings.apPass, pass, sizeof(settings.apPass) - 1);
  settings.apPass[sizeof(settings.apPass) - 1] = '\0';
  settings_save();
  return true;
}

void settings_apply_tz() {
  setenv("TZ", settings.tz, 1);
  tzset();
}

// ---- sleep history in NVS --------------------------------------------------
#if SLEEP_LOG_NVS

// Its own namespace, so a diagnostic that rewrites flash on every sleep cannot
// churn the page the real settings live on.
#define SLEEP_NS "sleephist"

void settings_log_sleep(const SleepRec *r) {
  if (!r) return;
  Preferences p;
  if (!p.begin(SLEEP_NS, /*readOnly=*/false)) return;

  SleepRec ring[SLEEP_LOG_NVS_N] = {};
  p.getBytes("ring", ring, sizeof(ring));      // zeroed if absent
  const uint8_t head = p.getUChar("head", 0) % SLEEP_LOG_NVS_N;

  ring[head] = *r;
  p.putBytes("ring", ring, sizeof(ring));
  p.putUChar("head", (uint8_t)((head + 1) % SLEEP_LOG_NVS_N));
  p.end();
}

void settings_dump_sleep_log() {
  Preferences p;
  // A read-only begin() FAILS when the namespace does not exist, which is the
  // normal state until the first sleep writes one. Report it rather than
  // returning quietly - a silent early return here is indistinguishable from
  // the feature being compiled out, and that ambiguity has already cost this
  // one feature two rounds of "it prints nothing".
  if (!p.begin(SLEEP_NS, /*readOnly=*/true)) {
    // Prove the write path before reporting "empty", because otherwise the two
    // possible meanings are indistinguishable - "the board never slept" and
    // "recording is broken" - and the whole point of this log is to settle
    // exactly that kind of question about the parked path.
    //
    // Runs only when the namespace is absent, so it costs nothing once real
    // records exist, and it CLEARS the namespace afterwards so the probe
    // leaves no fake entry to be mistaken for a sleep later.
    SleepRec probe = { .awakeMs = 0xA5A5A5A5, .mask = 0x5A5A5A5A,
                       .fruitless = 0x1234, .wakeCause = 0xAB, .canLevel = 0xCD };
    settings_log_sleep(&probe);

    Preferences q;
    bool ok = false;
    if (q.begin(SLEEP_NS, /*readOnly=*/false)) {
      SleepRec ring[SLEEP_LOG_NVS_N] = {};
      ok = q.getBytes("ring", ring, sizeof(ring)) == sizeof(ring) &&
           ring[0].awakeMs == 0xA5A5A5A5 && ring[0].mask == 0x5A5A5A5A &&
           ring[0].fruitless == 0x1234 && ring[0].wakeCause == 0xAB &&
           ring[0].canLevel == 0xCD;
      q.clear();                 // back to "never written", as we found it
      q.end();
    }
    Serial.printf("[..] sleep history empty - NVS write path %s\n",
                  ok ? "VERIFIED, so nothing ever slept"
                     : "BROKEN, so an empty history proves nothing");
    return;
  }

  SleepRec ring[SLEEP_LOG_NVS_N] = {};
  const size_t got = p.getBytes("ring", ring, sizeof(ring));
  const uint8_t head = p.getUChar("head", 0) % SLEEP_LOG_NVS_N;
  // Say so out loud. Returning silently here made "nothing has been recorded"
  // indistinguishable from "this code is not running at all", which is the
  // same ambiguity that made the black screen take three theories to pin down.
  if (got != sizeof(ring)) {
    Serial.println(F("[..] sleep history empty (nothing recorded yet)"));
    return;
  }

  // Oldest first, starting at head - that slot is the next to be written, so
  // it currently holds the oldest entry. awakeMs == 0 means never used.
  bool any = false;
  for (uint8_t i = 0; i < SLEEP_LOG_NVS_N; i++) {
    const SleepRec &s = ring[(head + i) % SLEEP_LOG_NVS_N];
    if (s.awakeMs == 0) continue;
    if (!any) {
      Serial.println(F("[..] recent sleeps (oldest first):"
                       "  awakeMs  wakeCause  canRx  mask  fruitless"));
      any = true;
    }
    Serial.printf("[..]   %8lu  %9u  %5u  0x%05lx  %9u\n",
                  (unsigned long)s.awakeMs, s.wakeCause, s.canLevel,
                  (unsigned long)s.mask, s.fruitless);
  }
  if (!any) Serial.println(F("[..] no sleeps recorded yet"));
}

#else
void settings_log_sleep(const SleepRec *) {}
void settings_dump_sleep_log() {}
#endif

bool settings_set_tz_offset(int8_t hours, bool dst) {
  if (hours < -12 || hours > 14) return false;
  settings.tzOffset = hours;
  settings.tzDst    = dst;
  buildTz();
  settings_save();
  settings_apply_tz();
  return true;
}
