// ============================================================================
// log_csv.cpp  —  the drive CSV: decoded values, a row every LOG_INTERVAL_MS.
// See log_priv.h.
// ============================================================================
#include "log_priv.h"
#include "logger.h"     // LOG_NORMAL / LOG_FULL
#include "config.h"
#include "calc.h"       // calc_fmt_deg
#include "obd.h"
#include "gps.h"
#include "rtc.h"        // clock_valid(): asks the clock, not one of its sources
#include "settings.h"
#include "webapi.h"     // web_is_up(), web_ap_refused()
#include <esp_heap_caps.h>
#include "SD_MMC.h"

// The CSV is staged in its batch and goes to the card on each flush (every
// LOG_FLUSH_INTERVAL_MS) - so that, encrypted, one flush is one record rather
// than one record per row. ~2 KB of rows per 2 s; a full batch is written
// early.
static constexpr size_t CSV_BUF_N = 4096;
static LogFile  csv;
static bool     pathDated = false;   // csv.path is a dated name, not a fallback number
static uint32_t driveOpenMs = 0;     // millis() when the CSV was created
static uint32_t lastSampleMs = 0;
static uint32_t lastFlushMs  = 0;
static bool     csvFailed = false;   // gave up on the file - see log_priv.h
RTC_DATA_ATTR static int nextDriveN = 0;

bool csvlog_init() { return logfile_alloc(csv, CSV_BUF_N); }
bool csvlog_failed() { return csvFailed; }
void csvlog_card_back() { csvFailed = false; }

const char *csvlog_path() { return csv.open ? csv.path : ""; }

// The loop's latest raw IMU sample, for gx/gy/gz - set from core 1.
static volatile float rawGx = 0, rawGy = 0, rawGz = 0;
static volatile bool  haveRawG = false;
void logger_note_raw_g(float gx, float gy, float gz) {
  rawGx = gx; rawGy = gy; rawGz = gz;
  haveRawG = true;
}

// What logging costs the loop core (asked 2026-10-09: does it slow the
// gauge?). The longest logger pass and the longest whole loop() pass since
// the previous CSV row, written as logMaxMs / loopMaxMs. A slow pass that
// lines up with a slow logger pass is the SD card stalling the screen; a
// frame at ~60 fps is ~16 ms. Two micros() a pass - always on.
static uint32_t logMaxUs = 0, loopMaxUs = 0;
void logger_note_loop_us(uint32_t us) { if (us > loopMaxUs) loopMaxUs = us; }
void csvlog_note_pass_us(uint32_t us) { if (us > logMaxUs) logMaxUs = us; }

// ---- the columns -------------------------------------------------------------
// One row is one pass over this table, and the header is the same table's
// names - so the two cannot drift apart, as they could while 51 columns were
// kept in step by hand. New columns go at the END: readers (the app, the
// scripts) find columns by name, but old files must keep their meaning.
//
// Each column writes its value or nothing. Empty rather than 0 when a value
// isn't available - a logged 0 is indistinguishable from a real reading
// during analysis. "Available" means CURRENT (obd_*_fresh), not "arrived
// once": after a dropout the raw flags stay true, and the CSV repeated the
// last value in every row - a flat line that looks exactly like a steady
// reading.

// What several columns read, taken once per row.
struct CsvRow {
  uint32_t now;
  char     utc[24];
  bool     fix;                 // lat and lon come from one fix
  char     lat[16], lon[16];
  bool     haveG;
  float    gx, gy, gz;
  bool     full;                // LOG_FULL: the raw hex columns
};

// Into the CSV's batch - what every column writes with.
static void put(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void put(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  logfile_vprintf(csv, fmt, ap);
  va_end(ap);
}

// A raw payload as hex, at LOG_FULL - one whole reply, copied out under the
// CAN task's lock (obd_copy_raw), so a newer one cannot land mid-cell.
static void putRaw(bool full, ObdRaw which) {
  if (!full) return;
  uint8_t b[48];
  const uint8_t n = obd_copy_raw(which, b, sizeof(b));
  for (uint8_t i = 0; i < n; i++) put(i ? " %02X" : "%02X", b[i]);
}

struct CsvCol {
  const char *name;
  void      (*write)(const CsvRow &r);
};

static const CsvCol COLS[] = {
  // Real wall-clock once the clock is trustworthy from ANY source - GNSS this
  // boot, the PCF85063 at boot, or a value carried through deep sleep. Blank
  // before that, so rows are never stamped with a bogus 1970 date that looks
  // real in a spreadsheet.
  {"utc",       [](const CsvRow &r) { put("%s", r.utc); }},
  {"ms",        [](const CsvRow &r) { put("%lu", (unsigned long)r.now); }},
  {"oilC",      [](const CsvRow &)  { if (obd_oil_fresh()) put("%d", obd.oilTempC); }},
  {"oilRaw",    [](const CsvRow &)  { if (obd_oil_fresh()) put("%u", obd.oilRaw); }},
  {"coolantC",  [](const CsvRow &)  { if (obd_coolant_fresh()) put("%d", obd.coolantC); }},
  {"boostBar",  [](const CsvRow &)  { if (obd_boost_fresh()) put("%.2f", obd.boostBar); }},
  {"gpsFix",    [](const CsvRow &r) { put("%d", r.fix ? 1 : 0); }},
  {"sats",      [](const CsvRow &)  { put("%u", gps.sats); }},
  {"speedKmh",  [](const CsvRow &r) { if (r.fix) put("%.1f", gps.speedKmh); }},
  {"lat",       [](const CsvRow &r) { if (r.fix) put("%s", r.lat); }},
  {"lon",       [](const CsvRow &r) { if (r.fix) put("%s", r.lon); }},
  // All THREE axes, raw from the IMU - not the projected values the g-meter
  // shows. The pod is mounted at ~45 deg, so no single raw axis is lateral or
  // longitudinal; keeping the raw vector plus GPS speed is what lets the
  // projection (and which axis is which) be worked out offline. Empty when
  // the IMU has never produced a sample.
  {"gx",        [](const CsvRow &r) { if (r.haveG) put("%.3f", r.gx); }},
  {"gy",        [](const CsvRow &r) { if (r.haveG) put("%.3f", r.gy); }},
  {"gz",        [](const CsvRow &r) { if (r.haveG) put("%.3f", r.gz); }},
  {"isoOk",       [](const CsvRow &) { put("%lu", (unsigned long)obd.isoOk); }},
  {"isoAborts",   [](const CsvRow &) { put("%lu", (unsigned long)obd.isoAborts); }},
  {"rxOverflows", [](const CsvRow &) { put("%lu", (unsigned long)obd.rxOverflows); }},
  {"busOff",      [](const CsvRow &) { put("%d", obd.busOff ? 1 : 0); }},
  {"busOffs",     [](const CsvRow &) { put("%lu", (unsigned long)obd.busOffs); }},
  // Indexed FL, FR, RL, RR, as obd.tyreBar is (ui.h's Wheel).
  {"t0bar", [](const CsvRow &) { if (obd_tyre_fresh(0)) put("%.2f", obd.tyreBar[0]); }},
  {"t1bar", [](const CsvRow &) { if (obd_tyre_fresh(1)) put("%.2f", obd.tyreBar[1]); }},
  {"t2bar", [](const CsvRow &) { if (obd_tyre_fresh(2)) put("%.2f", obd.tyreBar[2]); }},
  {"t3bar", [](const CsvRow &) { if (obd_tyre_fresh(3)) put("%.2f", obd.tyreBar[3]); }},
  {"t0c",   [](const CsvRow &) { if (obd_tyre_fresh(0)) put("%d", obd.tyreTempC[0]); }},
  {"t1c",   [](const CsvRow &) { if (obd_tyre_fresh(1)) put("%d", obd.tyreTempC[1]); }},
  {"t2c",   [](const CsvRow &) { if (obd_tyre_fresh(2)) put("%d", obd.tyreTempC[2]); }},
  {"t3c",   [](const CsvRow &) { if (obd_tyre_fresh(3)) put("%d", obd.tyreTempC[3]); }},
  // Raw DID 0xC00B payload as hex. This is the field that will actually
  // solve the wheel layout - space-separated so it stays one CSV column.
  // The two raw payload columns deliberately stay "last payload received",
  // not aged out: they are for decoding the format offline, and the decoded
  // columns beside them already say whether the reading is current.
  // The two raw hex columns are LOG_FULL only; below that they stay as
  // empty columns, so the CSV layout is the same at every level.
  {"tpmsRaw", [](const CsvRow &r) { putRaw(r.full, OBD_RAW_TPMS); }},
  // Raw DID 0xE001 payload as hex. The byte we decode as oil temp disagreed
  // with the dashboard by ~20 C on the first drive, and one byte of stored
  // history cannot settle which one is right. With the whole payload, a single
  // cold-start warm-up drive plus a few noted dash readings identifies the
  // correct offset offline. See the note in obd.h.
  {"oilRawPayload", [](const CsvRow &r) { putRaw(r.full, OBD_RAW_OIL); }},
  // With the GPX's per-point sat/hdop it answers whether the AP near the GNSS
  // antenna hurts the fix (see AP_DRIVE_OFF).
  {"apUp",   [](const CsvRow &) { put("%d", web_is_up() ? 1 : 0); }},
  // OBD speed: a speed where GPS has no fix.
  {"obdKmh", [](const CsvRow &) { if (obd_speed_fresh()) put("%d", obd.speedKmh); }},
  // The raw coolant sensor (PID 0x05). coolantC above is the dashboard's
  // value (DID 0xE001 byte 25) since 2026-10-03 - see COOLANT_FROM_E001.
  {"coolantPid05C", [](const CsvRow &) {
    if (calc_fresh(obd.havePid05, obd.pid05Ms, millis(), OBD_STALE_MS))
      put("%d", obd.coolantPid05C);
  }},
  // Absolute load, for calibrating a torque estimate (one full-throttle pull
  // maps its peak to the engine's rated torque), and the baro boost is zeroed
  // against.
  {"absLoadPct", [](const CsvRow &) { if (obd_absload_fresh()) put("%.1f", obd.absLoadPct); }},
  {"baroKpa",    [](const CsvRow &) { if (obd.haveBaro) put("%d", obd.baroKpa); }},
  // Accelerator pedal D, raw (x100/255 = %): 37 with the foot off; the
  // full-throttle value is what is still unknown.
  {"pedalRaw",   [](const CsvRow &) { if (obd_pedal_fresh()) put("%u", obd.pedalRaw); }},
  {"rpm",        [](const CsvRow &) { if (obd_rpm_fresh()) put("%d", obd.rpm); }},
  {"timingDeg",  [](const CsvRow &) { if (obd_timing_fresh()) put("%.1f", obd.timingDeg); }},
  {"iatC",       [](const CsvRow &) { if (obd_iat_fresh()) put("%d", obd.iatC); }},
  // 0 = no gear matched (shifting, slip, neutral); empty = rpm/speed stale.
  {"gear",       [](const CsvRow &) { const int g = log_gear(); if (g >= 0) put("%d", g); }},
  // Since the previous row. The pass writing this row is not in it yet - it
  // lands in the next row's figure. Both reset once the row is written.
  {"logMaxMs",   [](const CsvRow &) { put("%.1f", logMaxUs / 1000.0f); }},
  {"loopMaxMs",  [](const CsvRow &) { put("%.1f", loopMaxUs / 1000.0f); }},
  // PID 0x42: the ECU's supply.
  {"volts",      [](const CsvRow &) { if (obd_volts_fresh()) put("%.2f", obd.volts); }},
  // 2026-10-16 - see OBD_HZ1_PIDS. Fuel rate from the fast request; the rest
  // once a second, so most rows repeat.
  {"fuelLh",     [](const CsvRow &) { if (obd_fuel_lh_fresh()) put("%.2f", obd.fuelLh); }},
  {"lambda",     [](const CsvRow &) { if (obd_lambda_fresh()) put("%.3f", obd.lambda); }},
  {"lambdaCmd",  [](const CsvRow &) { if (obd_lambda_cmd_fresh()) put("%.3f", obd.lambdaCmd); }},
  {"ambientC",   [](const CsvRow &) { if (obd_ambient_fresh()) put("%d", obd.ambientC); }},
  {"fuelPct",    [](const CsvRow &) { if (obd_fuel_pct_fresh()) put("%.1f", obd.fuelPct); }},
  {"catC",       [](const CsvRow &) { if (obd_cat_fresh()) put("%.0f", obd.catC); }},
  {"railBar",    [](const CsvRow &) { if (obd_rail_fresh()) put("%.1f", obd.railBar); }},
  // Free internal RAM and the WiFi AP's refusals for want of it (2026-10-17:
  // after every drive the AP stayed down, unseen - see log_sink.cpp's ioBuf).
  // Against AP_MIN_INTERNAL_HEAP, this says how close a drive runs to it.
  {"ramKb",      [](const CsvRow &) {
    put("%u", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024));
  }},
  {"apRefused",  [](const CsvRow &) { put("%lu", (unsigned long)web_ap_refused()); }},
};

// Create the drive log on first use, not at boot.
//
// This matters because of deep sleep: parked, the board wakes every
// SLEEP_POLL_SEC, runs setup(), finds the bus silent and sleeps again. Opening
// a numbered file here at boot meant a brand new drive_NNNN.csv (header row and
// all, so the next boot skipped it and made another) on EVERY wake - about one
// every two seconds while parked, hitting the 9999 ceiling in a few hours and
// leaving the card full of junk. Worse, the free-number scan then costs
// thousands of exists() calls per wake, stretching each wake and burning the
// current that sleeping was supposed to save.
//
// "First use" means the first row after the first CAN frame - see
// csvlog_task(). Opening on the first row alone was not lazy enough: every
// bench boot and every parked wake with a silent bus still made a file of a
// few rows, and the card reached 245 of them.
static bool driveOpen() {
  const LogName name = lognames_new("drive_", ".csv", &nextDriveN, csv.path, sizeof(csv.path));
  pathDated = name == LOG_NAME_DATED;
  driveOpenMs = millis();
  if (name == LOG_NAME_NONE || !(csv.f = SD_MMC.open(csv.path, FILE_WRITE))) {
    Serial.println(F("[ERR] could not open a drive log file"));
    csvFailed = true;          // don't retry every pass once it's hopeless
    return false;
  }
  logfile_begin(csv);
  for (size_t i = 0; i < sizeof(COLS) / sizeof(COLS[0]); i++)
    logfile_printf(csv, "%s%s", i ? "," : "", COLS[i].name);
  logfile_puts(csv, "\r\n");
  logfile_commit(csv);
  csv.open = true;
  lognames_note_drive();
  Serial.printf("[OK] drive log %s%s\n", csv.path, csv.enc ? " (encrypted)" : "");
  return true;
}

// A drive that starts before any clock source is trusted gets a numbered file.
// The clock usually arrives a minute later with the first fix (leaving the
// garage), so rename the file then to the dated name it would have had -
// named after when it STARTED, not when the clock arrived. Rows written before
// that have a blank utc but keep ms, so their times can be recovered offline.
//
// One attempt, whatever the outcome: a card that refuses the rename would
// otherwise be retried on every row. The file never spans a deep sleep
// (logger_close() runs first), so millis() since opening is a true duration.
static void driveRenameDated() {
  pathDated = true;
  const time_t start = time(nullptr) - (time_t)((millis() - driveOpenMs) / 1000);
  char dated[sizeof(csv.path)];
  // The extension the open file already has, not the one a new file would
  // get: a key change closes every file before it applies, so the two agree,
  // but the name must follow the contents whatever happens.
  if (!lognames_dated("drive_", csv.enc ? ".csv" LOGCRYPT_SUFFIX : ".csv", start, dated, sizeof(dated))) return;
  logfile_commit(csv);
  csv.f.close();
  logfile_end(csv);
  const bool renamed = SD_MMC.rename(csv.path, dated);
  if (renamed) strlcpy(csv.path, dated, sizeof(csv.path));
  csv.f = SD_MMC.open(csv.path, FILE_APPEND);
  if (!csv.f) {
    Serial.printf("[ERR] could not reopen drive log %s - logging stopped\n", csv.path);
    csv.open = false;
    csvFailed = true;          // same give-up rule as driveOpen()
    return;
  }
  logfile_begin(csv);          // reopened: a new segment, a fresh key
  if (renamed) Serial.printf("[OK] drive log renamed to %s\n", csv.path);
  else         Serial.printf("[ERR] drive log rename failed, keeping %s\n", csv.path);
}

void csvlog_task() {
  const uint32_t now = millis();
  if (now - lastSampleMs < (log_idle() ? LOG_IDLE_INTERVAL_MS : LOG_INTERVAL_MS)) return;
  lastSampleMs = now;
  if (!csv.open) {
    // No file until the car is on, which everywhere else in the firmware
    // means "a CAN frame has been seen". Not a GNSS fix: a parked car near a
    // window has one too, and this is a car gauge, not a walk logger.
    if (obd.lastFrameMs == 0) return;
    if (csvFailed || !driveOpen()) return;
  } else if (!pathDated && clock_valid()) {
    driveRenameDated();
    if (!csv.open) return;
  }

  CsvRow r;
  r.now = now;
  r.full = settings.logLevel >= LOG_FULL;
  r.utc[0] = '\0';
  if (clock_valid()) {
    const time_t t = time(nullptr);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(r.utc, sizeof(r.utc), "%Y-%m-%dT%H:%M:%SZ", &tm);
  }
  // Same snapshot rule as the GPX: lat and lon must come from one fix.
  int32_t lat, lon;
  r.fix = gps.haveFix && gps_read_position(&lat, &lon, nullptr);
  if (r.fix) {
    calc_fmt_deg(r.lat, sizeof(r.lat), lat);
    calc_fmt_deg(r.lon, sizeof(r.lon), lon);
  }
  // The loop's latest raw IMU sample (logger_note_raw_g), not a read of our
  // own: the I2C bus is shared with touch and the RTC on core 1, and this
  // runs on core 0.
  r.gx = rawGx; r.gy = rawGy; r.gz = rawGz;
  r.haveG = haveRawG;

  // Straight into the batch, a column at a time; a batch that fills goes to
  // the card mid-row, which a reader never sees (records join seamlessly).
  for (size_t i = 0; i < sizeof(COLS) / sizeof(COLS[0]); i++) {
    if (i) logfile_puts(csv, ",");
    COLS[i].write(r);
  }
  logfile_puts(csv, "\r\n");
  logMaxUs = loopMaxUs = 0;

  if (now - lastFlushMs >= LOG_FLUSH_INTERVAL_MS) {
    lastFlushMs = now;
    logfile_commit(csv);   // bounds data loss when the ignition cuts power
  }
}

void csvlog_close() {
  if (!csv.open) return;
  logfile_commit(csv);
  csv.f.close();
  logfile_end(csv);
  csv.open = false;
}

void logger_dump_tail(size_t bytes) {
  LoggerSdLock lock;
  if (!csv.open) {
    Serial.printf("[..] no drive log open (card %s)\n", logger_card_ok() ? "mounted" : "NOT mounted");
    return;
  }
  logfile_commit(csv);
  if (csv.enc) {
    Serial.printf("[..] %s is encrypted - only the phone can read it\n", csv.path);
    return;
  }
  File f = SD_MMC.open(csv.path, FILE_READ);
  if (!f) { Serial.printf("[ERR] cannot read %s\n", csv.path); return; }
  const size_t sz = f.size();
  f.seek(sz > bytes ? sz - bytes : 0);
  Serial.printf("----- tail of %s (%u bytes) -----\n", csv.path, (unsigned)sz);
  uint8_t buf[128];
  size_t n;
  while ((n = f.read(buf, sizeof(buf))) > 0) Serial.write(buf, n);
  Serial.println(F("----- end -----"));
  f.close();
}
