// ============================================================================
// log_gpx.cpp  —  the GPX trip track, and the trip distance it alone can sum.
// See log_priv.h.
// ============================================================================
#include "log_priv.h"
#include "logger.h"     // LOG_TRIPS, logger_card_ok
#include "config.h"
#include "calc.h"       // GPS plausibility checks, shared with tests/
#include "gps.h"
#include "rtc.h"        // clock_valid(): asks the clock, not one of its sources
#include "settings.h"   // tripGapMin: the trip-reset gap is user-editable
#include "SD_MMC.h"
#include <sys/time.h>

// ISO 8601 with centiseconds. GPX and xsd:dateTime both allow a fractional
// part, and it is essential here: written to whole seconds, all ~25 points
// inside one second carried an IDENTICAL timestamp (measured: 42381 points
// collapsing into 1902 distinct stamps). Anything deriving speed or
// acceleration from consecutive points then divides by a zero time delta, so
// the 25 Hz data was unusable for exactly the analysis it exists for.
//
// The sub-second phase is arbitrary relative to true GPS epoch, because the
// clock is set from NMEA on whole-second boundaries - but the spacing is
// correct and monotonic, which is what those derivatives need.
static void isoStamp(char *out, size_t n) {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  struct tm tm;
  gmtime_r(&tv.tv_sec, &tm);
  const size_t len = strftime(out, n, "%Y-%m-%dT%H:%M:%S", &tm);
  if (len && len + 5 < n) snprintf(out + len, n - len, ".%02dZ",
                                   (int)(tv.tv_usec / 10000));
}

// Points accumulate in the batch and go to the card on GPX_FLUSH_MS (2 s) or
// when it is nearly full. At 25 Hz a write plus seek() per point would be 25
// SD operations a second, which stalls the render loop; batching keeps every
// sample with far fewer saves. 12 KB, not 2 KB: at 2 KB it filled every
// ~0.4 s at ~4.5 KB/s, and every save (data, tail, flush) cost the card
// ~13-20 ms: the logger's ~20 ms in most half-seconds on every drive of
// 2026-10-10/11 (logMaxMs), confirmed by a bench test of the card - writes
// into the file buffer ~0 ms, each flush 13 ms settled, 40-130 ms early.
static constexpr size_t GPX_BUF_N = 12288;
static LogFile  gpx;
static bool     gpxOpenFailed = false;  // stop retrying a hopeless open
static uint32_t gpxLastWriteMs = 0;
static bool     gpxDirty = false;      // points since the last commit
RTC_DATA_ATTR static int nextTripN = 0;
// Byte offset of the point insertion position, i.e. just past the last
// trackpoint and before the closing tags.
static uint32_t gpxTailPos = 0;

static const char GPX_TAIL[] = "</trkseg></trk></gpx>\n";

// After every batch: writes the closing tags and then rewinds to before them.
// The file on the card is therefore ALWAYS valid XML, which matters because
// the power is expected to die with the ignition - an unterminated GPX is
// rejected outright by most tools, so "close it properly on shutdown" is not
// good enough.
//
// Encrypted, no closing tags: a sealed record cannot be overwritten in place,
// and the readers add them (the app's cleanGpx, the PC tool).
static void gpxWriteTail(LogFile &lf) {
  if (lf.enc) return;
  gpxTailPos = lf.f.position();
  logfile_check_write(lf, lf.f.print(GPX_TAIL), sizeof(GPX_TAIL) - 1);
  lf.f.seek(gpxTailPos);
}

bool gpxlog_init() {
  gpx.afterDrain = gpxWriteTail;
  return logfile_alloc(gpx, GPX_BUF_N);
}

const char *gpxlog_path() { return gpx.path; }

// Flushed even with the batch empty: a point that did not fit drained it
// unflushed (logfile_printf), and this is what then puts it on the card.
static void gpxCommit() {
  if (!gpx.open) return;
  logfile_commit(gpx);
  gpxDirty = false;
}

// ---- trip distance ---------------------------------------------------------
// RTC_DATA_ATTR, so it survives deep sleep.
//
// The pod sleeps after five minutes of bus silence, so one journey with a fuel
// stop is several wakes. Resetting per wake would zero the trip at every stop,
// and resetting per GPX file would do the same, since each wake opens a new
// one. A power cycle DOES clear it, which is correct - that is someone
// unplugging the pod.
//
// float rather than double: 24 bits of mantissa still resolves ~0.06 m at
// 1000 km, far finer than the fixes being summed.
RTC_DATA_ATTR static float     tripMetres    = 0.0f;
RTC_DATA_ATTR static long long tripLastEpoch = 0;

// Start a new trip after a long enough gap between accepted points, so parking
// overnight does not carry yesterday's distance into today.
//
// Uses wall-clock, not millis(), which restarts on every wake and therefore
// cannot measure across a sleep at all. With no valid clock this does nothing
// and the distance keeps accumulating - the safe direction, since too much
// trip is obvious while a silent reset mid-journey is not.
//
// `recentThisWake` is true when millis() itself shows the previous accepted
// point was less than the trip gap ago. In that case there was provably no
// gap, whatever the wall clock says - and the wall clock CAN jump: seeded wrong
// from the RTC or the settings page, it steps when the first NMEA time sync
// corrects it, and a forward step of more than the gap used to zero the trip
// mid-journey. millis() is monotonic within a wake, so it wins when it can
// see; the wall clock is only consulted across a sleep, where millis() has
// restarted and cannot.
static void tripNoteTime(bool recentThisWake) {
  if (!clock_valid()) return;
  const long long nowS = (long long)time(nullptr);
  if (!recentThisWake && tripLastEpoch != 0 &&
      nowS - tripLastEpoch > (long long)settings.tripGapMin * 60) {
    tripMetres = 0.0f;
  }
  tripLastEpoch = nowS;
}

float logger_trip_km() { return tripMetres / 1000.0f; }

static void gpxOpen() {
  if (lognames_new("trip_", ".gpx", &nextTripN, gpx.path, sizeof(gpx.path)) != LOG_NAME_NONE &&
      (gpx.f = SD_MMC.open(gpx.path, FILE_WRITE))) {

    char stamp[24] = "";
    if (clock_valid()) {
      const time_t t = time(nullptr);
      struct tm tm;
      gmtime_r(&t, &tm);
      strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &tm);
    }
    // The gpxtpx namespace must be declared here or the per-point speed
    // extension is unreadable - viewers skip extensions in unknown namespaces.
    // Into the batch (empty at open) and out through gpxCommit(), so it is
    // sealed like the points when encrypting, and followed by the tail when
    // not.
    logfile_begin(gpx);
    logfile_printf(gpx,
                   "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                   "<gpx version=\"1.1\" creator=\"i30N-gauge\" "
                   "xmlns=\"http://www.topografix.com/GPX/1/1\" "
                   "xmlns:gpxtpx=\"http://www.garmin.com/xmlschemas/"
                   "TrackPointExtension/v1\">\n<trk><name>%s</name><trkseg>\n",
                   stamp[0] ? stamp : "trip");
    gpx.open = true;
    gpxCommit();
    Serial.printf("[OK] trip track %s%s\n", gpx.path, gpx.enc ? " (encrypted)" : "");
    return;
  }
  // Give up permanently, exactly as the CSV does. Without this, every
  // subsequent loop iteration retried - and a retry walks up to 9999
  // SD_MMC.exists() calls at roughly 0.5-2 ms each on 1-bit SDMMC, i.e. 5-20
  // seconds inside a single iteration. That never reaches
  // esp_task_wdt_reset(), so the 8 s watchdog fires and the board reboots,
  // then does it again on the next fix: a permanent reboot loop, triggered by
  // nothing worse than a full card.
  gpxOpenFailed = true;
  Serial.println(F("[ERR] could not open a GPX trip file - GPX disabled"));
}

void gpxlog_card_back() { gpxOpenFailed = false; }
bool gpxlog_failed() { return gpxOpenFailed; }

// Two jobs, in this order: decide whether the current fix is a trustworthy
// point (rate, quality, plausibility), then - only if there is a card - write
// it.
//
// The card checks used to come FIRST, which silently tied the trip distance to
// the SD card: with no card, a full card or a failed open, every call returned
// before the gates, so the trip read 0.0 km for the whole drive. No card is the
// normal bench configuration. Deciding what counts as a real point has nothing
// to do with whether it can be stored.
void gpxlog_task() {
  // The time half of "commit on size or on time", up here, before anything
  // can return. Down by the point it only ran when a point was accepted: a
  // fix lost in a tunnel or a garage left up to 2 s of points (12 KB) in RAM
  // for as long as the fix stayed away - lost if the power went meanwhile.
  const uint32_t now = millis();
  if (gpxDirty && now - gpxLastWriteMs >= GPX_FLUSH_MS) {
    gpxLastWriteMs = now;
    gpxCommit();
  }
  if (!gps.haveFix) return;            // nothing worth recording yet

  // ONE snapshot of the fix, used for every decision below and for the point
  // written. The GNSS runs on core 0, so reading gps.lat1e7/lon1e7 afresh at
  // each step let a new fix land between the jump check and the write.
  // gps_read_fix() also refuses until a position has actually been
  // published, which is what stopped (0,0) being accepted on the first fix
  // after a wake.
  //
  // And each fix ONCE. This ran every logger pass and took whatever position
  // was there, new or not: with the receiver at 1 Hz (the carrier PCB,
  // 2026-10-19) every fix was written ~25 times, and the jump check timed the
  // next real step from the last copy - a second's travel in 40 ms, ~500 m/s,
  // refused - so the trip read a fraction of the distance. RMC and GGA both
  // publish, so one epoch can bump seq twice with the same time: that one is
  // skipped too (as the .vbo does).
  GpsFix fix;
  if (!gps_read_fix(&fix) || !fix.timeOk) return;
  static uint32_t lastSeq = 0, lastFixMs = UINT32_MAX;
  if (fix.seq == lastSeq || fix.fixMs == lastFixMs) return;
  lastSeq = fix.seq;
  lastFixMs = fix.fixMs;
  const int32_t posLat = fix.lat1e7, posLon = fix.lon1e7;
  const float   posAlt = fix.altM;

  // The gate's clock is the FIX's time, not when this pass happened to run:
  // a pass lands up to LOGGER_PASS_MS (and a save) after the fix, which is
  // most of a 40 ms step at 25 Hz. Ms of day made monotonic across midnight
  // (and never 0, the gate's "none yet").
  static uint32_t dayBase = 0, prevFixMs = 0;
  if (fix.fixMs + 43200000UL < prevFixMs) dayBase += 86400000UL;   // past midnight
  prevFixMs = fix.fixMs;
  const uint32_t fixClock = dayBase + fix.fixMs + 1;

  // Rate, quality and plausibility: calc_gpx_gate() (calc.h, host-tested).
  // The gate runs with or without a card - deciding what counts as a real
  // point has nothing to do with whether it can be stored, and a gate that
  // only advanced on a written point never engaged its rate limiter without
  // a card, which broke the jump check (see calc.h).
  static CalcGpxGate gate;
  const CalcGpxLimits lim = {
    GPX_POINT_MS, GPX_IDLE_POINT_MS, GPS_MIN_SATS_LOG, GPS_MAX_HDOP_LOG,
    GPX_SEGMENT_GAP_MS, GPX_MAX_JUMP_KMH, GPX_MAX_REJECT_RUN,
    // One definition of "stationary" in the firmware, not two.
    LOG_IDLE_SPEED_KMH,
    (uint32_t)(settings.tripGapMin * 60000UL),
  };
  CalcGpxStep step;
  if (!calc_gpx_gate(&gate, &lim, fixClock, log_idle(), gps.sats, gps.hdop,
                     posLat, posLon, gps.speedKmh, &step))
    return;

  // Trip distance, summed HERE and only here - past every gate, so a tunnel
  // spike that would have added kilometres nobody drove has already been
  // rejected. That is the whole reason this lives in the logger rather than
  // somewhere more obvious like gps.cpp: this is the only place in the
  // firmware that knows a fix is trustworthy.
  tripMetres += step.addM;
  tripNoteTime(step.recent);

  // ---- everything below writes the point, and needs a card ----------------
  if (!log_may_write()) return;        // no card, or writes given up on
  if (settings.logLevel < LOG_TRIPS) return;   // logging off: count, don't write
  if (gpxOpenFailed) return;           // see gpxOpen(): do not retry forever
  if (!gpx.open) gpxOpen();            // lazily, on the first accepted point
  if (!gpx.open) return;

  // Break the track where the fix was lost, so a viewer doesn't join the last
  // point before a tunnel straight to the first one after it and draw a route
  // that was never driven.
  //
  // Emitted HERE, after every gate and immediately before the point itself,
  // because it must only be written when a point actually follows it. With the
  // split before the gates it was governed by the last point's time, which the
  // gates hold stale - so a spell of poor reception appended
  // "</trkseg><trkseg>" on every loop iteration, thousands of empty segments
  // and kilobytes a second of nothing, with SD flushes to match.
  if (step.gap) logfile_puts(gpx, "</trkseg><trkseg>\n");

  char stamp[32] = "";
  if (clock_valid()) isoStamp(stamp, sizeof(stamp));

  // ele/time/sat/hdop are standard GPX 1.1 trackpoint elements. Speed is NOT:
  // <speed> was a GPX 1.0 element and was dropped from the 1.1 schema, so in a
  // file that declares 1.1 (as this one does) compliant viewers ignore it and
  // fall back to deriving speed from consecutive positions. At 20 Hz that
  // means differentiating 5 cm of movement against metres of GPS noise, which
  // is worthless - which is exactly how it looked: a spike at the start and
  // then nothing. The Garmin TrackPointExtension below is what tools actually
  // read (Garmin, Strava, GPXSee, gpx.studio), and its unit is m/s.
  // Coordinates as exact decimal text straight from the int32, rather than
  // %.7f on a converted float - no rounding anywhere in the output path.
  char latS[16], lonS[16];
  calc_fmt_deg(latS, sizeof(latS), posLat);
  calc_fmt_deg(lonS, sizeof(lonS), posLon);

  logfile_printf(gpx,
      "<trkpt lat=\"%s\" lon=\"%s\"><ele>%.1f</ele>"
      "<time>%s</time><sat>%u</sat><hdop>%.1f</hdop>"
      "<extensions><gpxtpx:TrackPointExtension>"
      "<gpxtpx:speed>%.2f</gpxtpx:speed>"
      "</gpxtpx:TrackPointExtension></extensions></trkpt>\n",
      latS, lonS, posAlt, stamp, gps.sats, gps.hdop,
      gps.speedKmh / 3.6f);             // m/s, per the TrackPointExtension schema
  gpxDirty = true;

  // Commit on size, or on time - checked here too, so a steady 25 Hz commits
  // on the pass of the point rather than the next one.
  if (gpx.len > GPX_BUF_N - 256 || now - gpxLastWriteMs >= GPX_FLUSH_MS) {
    gpxLastWriteMs = now;
    gpxCommit();
  }
}

void gpxlog_close() {
  if (!gpx.open) return;
  gpxCommit();                   // don't lose the batch still in RAM
  if (!gpx.enc) {
    // Advance past the tail we keep rewinding over, so the closing tags
    // are the real end of the file, then close.
    gpx.f.seek(gpxTailPos);
    logfile_check_write(gpx, gpx.f.print(GPX_TAIL), sizeof(GPX_TAIL) - 1);
  }
  gpx.f.flush();
  gpx.f.close();
  logfile_end(gpx);
  gpx.open = false;
}
