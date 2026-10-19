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
static uint32_t gpxLastMs = 0;
static uint32_t gpxLastWriteMs = 0;
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
  lf.f.print(GPX_TAIL);
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
  if (!gps.haveFix) return;            // nothing worth recording yet

  // ONE snapshot of the position, used for every decision below and for the
  // point written. The GNSS runs on core 0 and publishes at 25 Hz, so reading
  // gps.lat1e7/lon1e7 afresh at each step let a new fix land between the jump
  // check and the write - the point recorded was not the point that was
  // checked. gps_read_position() also refuses until a position has actually
  // been published, which is what stopped (0,0) being accepted on the first
  // fix after a wake.
  int32_t posLat, posLon;
  float   posAlt;
  if (!gps_read_position(&posLat, &posLon, &posAlt)) return;

  const uint32_t now = millis();

  // RATE LIMIT FIRST. Everything below judges a candidate against the last
  // point we WROTE, so it has to run at the write rate, not the loop rate.
  //
  // This ordering is load-bearing, and getting it wrong broke the jump check
  // outright: with the limiter last, the check ran every loop iteration (~5 ms)
  // and refreshed its reference point each time, while gps.lat/lon only change
  // every GPS_NAV_RATE_MS (40 ms). So 40 ms of travel got divided by a 5 ms
  // interval - roughly an 8x overstatement - and the effective rejection
  // threshold collapsed from 250 km/h to about 31. Ordinary driving was being
  // thrown away. The offline simulation missed it because it replayed points
  // that had already been written, i.e. already 40 ms apart.
  const uint32_t interval = log_idle() ? GPX_IDLE_POINT_MS : GPX_POINT_MS;
  if (gpxLastMs != 0 && now - gpxLastMs < interval) return;

  // Refuse points the receiver does not actually stand behind. On the first
  // real drive 315 points (0.8 %) went in with <sat>0</sat> and hdop 99.99 -
  // a frozen position with a stale speed, written for up to GPS_FIX_STALE_MS
  // after the fix was lost, in runs of 52 points. In a viewer that reads as
  // the car stopping dead and then teleporting.
  //
  // Guarded by sawSats so this can only ever suppress points on a module that
  // has proven it reports satellite counts: if GGA were absent, sats would sit
  // at 0 forever and this would silently disable logging altogether.
  static bool sawSats = false;
  if (gps.sats > 0) sawSats = true;
  if (sawSats && !calc_fix_quality_ok(gps.sats, gps.hdop,
                                      GPS_MIN_SATS_LOG, GPS_MAX_HDOP_LOG))
    return;

  // Physical plausibility check, and the one that actually fixes tunnels.
  //
  // Coming out of a tunnel the track sprayed across the map in
  // straight spikes. Quality flags alone do NOT catch it: the worst points
  // carried 3-4 satellites but plausible-looking HDOP of 1.5-2.2, and on the
  // logged drive filtering on sats and HDOP still left 35 jumps with a worst
  // case of 12920 km/h. A car cannot move that fast, so the distance from the
  // last ACCEPTED point is the reliable discriminator.
  //
  // Across a real gap we cannot judge anything - the car genuinely moved while
  // we had no fix - so a point after GPX_SEGMENT_GAP_MS is always accepted and
  // the segment splitter below breaks the line instead of drawing through it.
  // The consecutive-reject limit is an escape hatch: if the reference point
  // itself is bad, refusing forever would throw away the rest of the trip.
  static int32_t lastOkLat = 0, lastOkLon = 0;   // degrees x 1e7, as stored
  static uint32_t lastOkMs = 0;
  static uint16_t rejectRun = 0;
  if (lastOkMs != 0 && now - lastOkMs <= GPX_SEGMENT_GAP_MS) {
    const float kmh = calc_implied_kmh(lastOkLat, lastOkLon,
                                       posLat, posLon,
                                       (now - lastOkMs) / 1000.0f);
    if (kmh > GPX_MAX_JUMP_KMH && rejectRun < GPX_MAX_REJECT_RUN) {
      rejectRun++;
      return;
    }
  }
  // Trip distance, summed HERE and only here - past every gate above, so a
  // tunnel spike that would have added kilometres nobody drove has already
  // been rejected. That is the whole reason this lives in the logger rather
  // than somewhere more obvious like gps.cpp: this is the only place in the
  // firmware that knows a fix is trustworthy.
  //
  // Only across a continuous stretch. Beyond GPX_SEGMENT_GAP_MS the car
  // genuinely moved while we had no fix, and counting that leg would add a
  // straight line between wherever the signal died and wherever it returned -
  // the same reason the segment splitter below breaks the track there.
  //
  // And only while actually moving. Standing at lights with a fix, the
  // position wanders by centimetres to metres from one point to the next, and
  // points are accepted at 25 Hz for minutes before idle throttling starts -
  // so summing every leg added hundreds of metres without the car moving.
  // GNSS speed is Doppler-derived and far steadier near zero than position
  // is, so it is the right discriminator. Reuses LOG_IDLE_SPEED_KMH so the
  // firmware has one definition of "stationary", not two.
  if (lastOkMs != 0 && now - lastOkMs <= GPX_SEGMENT_GAP_MS &&
      gps.speedKmh >= LOG_IDLE_SPEED_KMH) {
    tripMetres += calc_distance_m(lastOkLat, lastOkLon,
                                  posLat, posLon);
  }
  tripNoteTime(lastOkMs != 0 &&
               now - lastOkMs < (uint32_t)settings.tripGapMin * 60000UL);

  rejectRun  = 0;
  lastOkLat  = posLat;
  lastOkLon  = posLon;
  lastOkMs   = now;

  // ---- everything below writes the point, and needs a card ----------------
  //
  // gpxLastMs advances HERE, with or without a card. It is the rate limiter's
  // reference, and if it only moved when a point was written, a card-less run
  // would never engage the limiter - the jump check would then run at loop
  // rate and divide 40 ms of travel by ~5 ms, the 8x overstatement described
  // at the top of this function. Capture the gap it implies first, for the
  // segment split below.
  const bool gapSinceLast = gpxLastMs != 0 && now - gpxLastMs > GPX_SEGMENT_GAP_MS;
  gpxLastMs = now;

  if (!logger_card_ok()) return;       // no card
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
  // split before the gates it was governed by gpxLastMs, which the gates hold
  // stale - so a spell of poor reception appended "</trkseg><trkseg>" on every
  // loop iteration, thousands of empty segments and kilobytes a second of
  // nothing, with SD flushes to match.
  if (gapSinceLast) logfile_puts(gpx, "</trkseg><trkseg>\n");

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

  // Commit on size or on time, whichever comes first.
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
    gpx.f.print(GPX_TAIL);
  }
  gpx.f.flush();
  gpx.f.close();
  logfile_end(gpx);
  gpx.open = false;
}
