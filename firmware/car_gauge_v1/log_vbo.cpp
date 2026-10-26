// ============================================================================
// log_vbo.cpp  —  the .vbo track session log. See log_priv.h.
// ============================================================================
#include "log_priv.h"
#include "logger.h"
#include "config.h"
#include "calc.h"       // calc_vbo_*: the format's units
#include "obd.h"
#include "gps.h"
#include "rtc.h"        // clock_valid()
#include "laps.h"       // the track session this log follows
#include "SD_MMC.h"

// ---- VBO track session ------------------------------------------------------
// A Racelogic .vbo per track session, for Circuit Tools (free) and other
// motorsport analysers, which plot channels against distance and split laps -
// things neither the GPX nor the CSV can feed them. Only in track mode: the
// file opens once the lap gate is set AND its direction learned (so the
// start/finish line can go into the header), and closes on STOP or sleep.
// One row per GNSS fix, 25 Hz, ~430 bytes: ~39 MB an hour.
//
// Format per Racelogic's .VBO page and real VBOX files (researched
// 2026-10-05): CRLF lines; lat/long in TOTAL arc-minutes with longitude
// positive WEST (calc_vbo_minutes); time HHMMSS.SS UTC; velocity km/h;
// extra channels named in [header], unit in [channel units] (extras only, as
// VBOX HD2 files do), underscored in [column names], values in exponent form.
// Opened 2026-10-16 (a session with laps): Circuit Tools 3.2 reads it, as
// does HackTheTrack (laps found). Circuit Tools takes no [laptiming] line for
// a circuit it does not know - a real Racelogic file's neither - and asks for
// one; lapvisor 0.3 misreads that section in both.
static constexpr size_t VBO_BUF_N = 4096;
static LogFile  vbo;
static bool     vboFailed = false;
static uint32_t vboLastWriteMs = 0, vboLastFixMs = UINT32_MAX;
static bool     vboDirty = false;      // rows since the last commit
static FixCursor vboCursor = {};       // our place in the queued fixes
static uint32_t vboLost = 0;
RTC_DATA_ATTR static int nextTrackN = 0;

// The car's channels, as each fix is queued (on the GNSS task - gps.h): a
// row written late, after a card stall, carries what the car did at its fix,
// not at the moment the row reached the file - for lap analysis, the engine
// against the position is the point. Engine channels have no "missing" value
// in a .vbo; a stale one is written as 0 rather than repeating a frozen
// reading.
enum {
  CAR_BOOST, CAR_OIL, CAR_COOL, CAR_PEDAL, CAR_TORQUE, CAR_GLAT, CAR_GLONG, CAR_RPM,
  CAR_TIMING, CAR_IAT, CAR_TYRE_BAR, CAR_TYRE_C = CAR_TYRE_BAR + 4, CAR_GEAR = CAR_TYRE_C + 4,
  CAR_FUEL_LH, CAR_LAMBDA, CAR_LAMBDA_CMD, CAR_AMBIENT, CAR_FUEL_PCT, CAR_CAT, CAR_RAIL, CAR_N
};
static_assert(CAR_N <= GPS_SAMPLE_CAR, "the .vbo's car channels fit a GpsSample");

static float gLateral = 0, gLongitudinal = 0;

static void vboSampleHook(GpsSample *s) {
  float *c = s->car;
  c[CAR_BOOST]  = obd_boost_fresh()   ? obd.boostBar : 0.0f;
  c[CAR_OIL]    = obd_oil_fresh()     ? (float)obd.oilTempC : 0.0f;
  c[CAR_COOL]   = obd_coolant_fresh() ? (float)obd.coolantC : 0.0f;
  c[CAR_PEDAL]  = obd_pedal_fresh()
      ? constrain(((int)obd.pedalRaw - PEDAL_RAW_IDLE) * 100.0f /
                  (PEDAL_RAW_FULL - PEDAL_RAW_IDLE), 0.0f, 100.0f) : 0.0f;
  c[CAR_TORQUE] = obd_absload_fresh() ? TORQUE_NM_FROM_LOAD(obd.absLoadPct) : 0.0f;
  c[CAR_GLAT]   = gLateral;
  c[CAR_GLONG]  = gLongitudinal;
  c[CAR_RPM]    = obd_rpm_fresh()     ? (float)obd.rpm : 0.0f;
  c[CAR_TIMING] = obd_timing_fresh()  ? obd.timingDeg : 0.0f;
  c[CAR_IAT]    = obd_iat_fresh()     ? (float)obd.iatC : 0.0f;
  // Tyres are polled once a minute, so most rows repeat the last value;
  // hot pressures over a session are what matters. FL, FR, RL, RR, as
  // obd.tyreBar is (ui.h's Wheel).
  for (int w = 0; w < 4; w++) {
    const bool ok = obd_tyre_fresh(w);
    c[CAR_TYRE_BAR + w] = ok ? obd.tyreBar[w] : 0.0f;
    c[CAR_TYRE_C + w]   = ok ? (float)obd.tyreTempC[w] : 0.0f;
  }
  const int gear = log_gear();
  c[CAR_GEAR] = (float)(gear > 0 ? gear : 0);    // 0 = none or not current
  // 2026-10-16 (OBD_HZ1_PIDS): fuel rate at the fast request's rate, the
  // rest once a second.
  c[CAR_FUEL_LH]    = obd_fuel_lh_fresh()    ? obd.fuelLh : 0.0f;
  c[CAR_LAMBDA]     = obd_lambda_fresh()     ? obd.lambda : 0.0f;
  c[CAR_LAMBDA_CMD] = obd_lambda_cmd_fresh() ? obd.lambdaCmd : 0.0f;
  c[CAR_AMBIENT]    = obd_ambient_fresh()    ? (float)obd.ambientC : 0.0f;
  c[CAR_FUEL_PCT]   = obd_fuel_pct_fresh()   ? obd.fuelPct : 0.0f;
  c[CAR_CAT]        = obd_cat_fresh()        ? obd.catC : 0.0f;
  c[CAR_RAIL]       = obd_rail_fresh()       ? obd.railBar : 0.0f;
}

bool vbolog_init() {
  gps_set_sample_hook(vboSampleHook);
  return logfile_alloc(vbo, VBO_BUF_N);
}
bool vbolog_failed() { return vboFailed; }
void vbolog_card_back() { vboFailed = false; }

const char *vbolog_path() { return vbo.open ? vbo.path : ""; }

void logger_note_g(float lateral, float longitudinal) {
  gLateral = lateral;
  gLongitudinal = longitudinal;
}

void vbolog_close() {
  if (!vbo.open) return;
  logfile_commit(vbo);
  vbo.f.close();
  logfile_end(vbo);
  vbo.open = false;
  Serial.printf("[OK] track log %s closed\n", vbo.path);
}

static void vboOpen() {
  int32_t gLat, gLon;
  float dx, dy;
  if (!laps_gate(&gLat, &gLon, &dx, &dy)) return;
  if (lognames_new("track_", ".vbo", &nextTrackN, vbo.path, sizeof(vbo.path)) == LOG_NAME_NONE ||
      !(vbo.f = SD_MMC.open(vbo.path, FILE_WRITE))) {
    Serial.println(F("[ERR] could not open a track log - VBO disabled for this session"));
    vboFailed = true;           // same give-up rule as the GPX: never retry per loop
    return;
  }
  char created[40] = "01/01/1980 at 00:00:00";
  if (clock_valid()) {
    const time_t t = time(nullptr);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(created, sizeof(created), "%d/%m/%Y at %H:%M:%S", &tm);
  }
  // The header goes through the batch too, so it is sealed when encrypting.
  logfile_begin(vbo);
  logfile_printf(vbo, "File created on %s\r\n\r\n", created);
  logfile_puts(vbo,
         "[header]\r\nsatellites\r\ntime\r\nlatitude\r\nlongitude\r\n"
         "velocity kmh\r\nheading\r\nheight\r\n"
         "Boost Pressure\r\nOil Temperature\r\nCoolant Temperature\r\n"
         "Throttle Pedal\r\nEngine Torque Estimate\r\n"
         "Lateral G IMU\r\nLongitudinal G IMU\r\nEngine RPM\r\n"
         "Ignition Timing\r\nIntake Air Temperature\r\nLap Number\r\n"
         "Tyre Pressure FL\r\nTyre Pressure FR\r\nTyre Pressure RL\r\nTyre Pressure RR\r\n"
         "Tyre Temperature FL\r\nTyre Temperature FR\r\n"
         "Tyre Temperature RL\r\nTyre Temperature RR\r\nGear\r\n"
         "Fuel Rate\r\nLambda\r\nLambda Target\r\nAmbient Temperature\r\n"
         "Fuel Level\r\nCatalyst Temperature\r\nFuel Rail Pressure\r\n\r\n"
         "[channel units]\r\nbar\r\nC\r\nC\r\n%\r\nNm\r\ng\r\ng\r\nrpm\r\n"
         // A blank unit line would end the section, so the unitless
         // channels get a word instead.
         "deg\r\nC\r\ncount\r\nbar\r\nbar\r\nbar\r\nbar\r\nC\r\nC\r\nC\r\nC\r\ngear\r\n"
         "l/h\r\nlambda\r\nlambda\r\nC\r\n%\r\nC\r\nbar\r\n\r\n"
         // No brackets in comments: they hang Circuit Tools.
         "[comments]\r\ni30 N gauge pod, ESP32-S3 with u-blox M9N\r\n"
         "Log Rate (Hz) : 25.00\r\n"
         "Torque is estimated from absolute load, not measured\r\n"
         "Longitudinal G IMU is positive when accelerating\r\n\r\n");
  // The pod's own start/finish gate, so the analyser splits laps at the same
  // line: TRACK_GATE_HALF_M either side of its centre, across the direction
  // of travel. Longitude first here - the reverse of the data columns.
  int32_t aLat, aLon, bLat, bLon;
  calc_offset_ll(gLat, gLon, -dy * TRACK_GATE_HALF_M,  dx * TRACK_GATE_HALF_M, &aLat, &aLon);
  calc_offset_ll(gLat, gLon,  dy * TRACK_GATE_HALF_M, -dx * TRACK_GATE_HALF_M, &bLat, &bLon);
  char aLonS[16], aLatS[16], bLonS[16], bLatS[16];
  calc_vbo_minutes(aLonS, sizeof(aLonS), aLon, true);
  calc_vbo_minutes(aLatS, sizeof(aLatS), aLat, false);
  calc_vbo_minutes(bLonS, sizeof(bLonS), bLon, true);
  calc_vbo_minutes(bLatS, sizeof(bLatS), bLat, false);
  logfile_printf(vbo, "[laptiming]\r\nStart        %s %s %s %s \xC2\xAC  Start / Finish\r\n\r\n",
                 aLonS, aLatS, bLonS, bLatS);
  logfile_puts(vbo,
         "[column names]\r\nsats time lat long velocity heading height "
         "Boost_Pressure Oil_Temperature Coolant_Temperature Throttle_Pedal "
         "Engine_Torque_Estimate Lateral_G_IMU Longitudinal_G_IMU Engine_RPM "
         "Ignition_Timing Intake_Air_Temperature Lap_Number "
         "Tyre_Pressure_FL Tyre_Pressure_FR Tyre_Pressure_RL Tyre_Pressure_RR "
         "Tyre_Temperature_FL Tyre_Temperature_FR "
         "Tyre_Temperature_RL Tyre_Temperature_RR Gear "
         "Fuel_Rate Lambda Lambda_Target Ambient_Temperature "
         "Fuel_Level Catalyst_Temperature Fuel_Rail_Pressure\r\n\r\n"
         "[data]\r\n");
  logfile_commit(vbo);
  vbo.open = true;
  vboLastWriteMs = millis();
  Serial.printf("[OK] track log %s%s\n", vbo.path, vbo.enc ? " (encrypted)" : "");
}

void vbolog_task() {
  LapInfo li;
  laps_info(&li);
  if (li.state < LAP_OUT_LAP) {         // no session, or the line not found yet
    vbolog_close();
    if (li.state == LAP_IDLE) vboFailed = false;   // a new session may try again
    vboCursor.started = false;          // a file starts with the fixes after it
    return;
  }
  if (!vbo.open && !vboFailed) vboOpen();
  if (!vbo.open) {
    vboCursor.started = false;
    return;
  }

  const uint32_t now = millis();
  // Every fix since the last pass, each with the car as it was at it
  // (vboSampleHook) - after a card stall, late, but all of them.
  // The lap being driven: 1, 2, ... (0 only on an out-lap after a wake
  // mid-session) - the pod's own split, to check the analyser's against.
  // Read now, not per fix: it changes once a lap.
  const float lap = li.state == LAP_TIMING ? (float)(li.laps + 1) : 0.0f;
  const uint32_t lostBefore = vboLost;
  GpsSample s;
  while (gps_next_sample(&vboCursor, &s, &vboLost)) {
    const GpsFix &f = s.fix;
    // One row per NEW fix: RMC and GGA both publish, so one 40 ms epoch is
    // queued twice with the same time - the second would duplicate the row.
    if (!f.timeOk || f.fixMs == vboLastFixMs ||
        !calc_fix_quality_ok(s.sats, s.hdop, GPS_MIN_SATS_LOG, GPS_MAX_HDOP_LOG))
      continue;
    vboLastFixMs = f.fixMs;
    char t[16], lat[16], lon[16];
    calc_vbo_time(t, sizeof(t), f.fixMs);
    calc_vbo_minutes(lat, sizeof(lat), f.lat1e7, false);
    calc_vbo_minutes(lon, sizeof(lon), f.lon1e7, true);
    const float *c = s.car;
    logfile_printf(vbo,
        "%03u %s %s %s %07.3f %06.2f %+09.2f %+.6E %+.6E %+.6E %+.6E %+.6E %+.6E %+.6E %+.6E"
        " %+.6E %+.6E %+.6E %+.6E %+.6E %+.6E %+.6E %+.6E %+.6E %+.6E %+.6E %+.6E"
        " %+.6E %+.6E %+.6E %+.6E %+.6E %+.6E %+.6E\r\n",
        s.sats, t, lat, lon, s.speedKmh, s.courseDeg, f.altM,
        c[CAR_BOOST], c[CAR_OIL], c[CAR_COOL], c[CAR_PEDAL], c[CAR_TORQUE],
        c[CAR_GLAT], -c[CAR_GLONG], c[CAR_RPM], c[CAR_TIMING], c[CAR_IAT], lap,
        c[CAR_TYRE_BAR], c[CAR_TYRE_BAR + 1], c[CAR_TYRE_BAR + 2], c[CAR_TYRE_BAR + 3],
        c[CAR_TYRE_C], c[CAR_TYRE_C + 1], c[CAR_TYRE_C + 2], c[CAR_TYRE_C + 3],
        c[CAR_GEAR], c[CAR_FUEL_LH], c[CAR_LAMBDA], c[CAR_LAMBDA_CMD], c[CAR_AMBIENT],
        c[CAR_FUEL_PCT], c[CAR_CAT], c[CAR_RAIL]);
    vboDirty = true;
  }
  if (vboLost != lostBefore)
    Serial.printf("[ERR] track log: %lu fixes lost - the logger fell >5 s behind\n",
                  (unsigned long)(vboLost - lostBefore));
  // A full batch goes to the card by itself (logfile_printf drains it); the
  // flush - what survives a power cut - on VBO_FLUSH_MS, and only after rows.
  // It used to commit every second whether anything was written or not, and
  // on every near-full batch: at 25 Hz that was a flush every ~0.4 s.
  if (vboDirty && now - vboLastWriteMs >= VBO_FLUSH_MS) {
    vboLastWriteMs = now;
    vboDirty = false;
    logfile_commit(vbo);
  }
}
