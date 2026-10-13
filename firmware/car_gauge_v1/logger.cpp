#include "logger.h"
#include "config.h"
#include "calc.h"       // GPS plausibility checks, shared with tests/
#include "obd.h"
#include "gps.h"
#include "rtc.h"        // clock_valid(): asks the clock, not one of its sources
#include "settings.h"   // tripGapMin: the trip-reset gap is user-editable
#include "webapi.h"     // web_is_up(), for the apUp column
#include "laps.h"       // the track session the .vbo log follows
#include "logcrypt.h"   // the encrypted log format
#include <Arduino.h>
#include <esp_random.h>
#include "FS.h"
#include "SD_MMC.h"
#include <time.h>
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

// FAT file times in UTC, not local time. FatFs asks get_fattime() for the
// stamp, and ESP-IDF's version uses localtime() - which is what the FAT spec
// says, but Linux (and Android) mount vfat reading the stamps as UTC, so on
// the PC every file showed 2 h late in CEST (2026-10-04: a log whose last row
// was 15:12:53Z, 17:12 local, listed as 19:12). The card is read on Linux, so
// stamp UTC. Windows would now show them 2 h early.
//
// Reached through the linker: scripts/build.sh passes -Wl,--wrap=get_fattime,
// which sends FatFs's call here instead of to diskio.c (defining a plain
// get_fattime would collide with it, as that object also holds the disk I/O
// FatFs needs). A build without the flag - the Arduino IDE - just leaves this
// unused and keeps local-time stamps.
extern "C" uint32_t __wrap_get_fattime(void) {
  const time_t t = time(nullptr);
  struct tm tm;
  gmtime_r(&t, &tm);
  if (tm.tm_year < 80) return (1u << 21) | (1u << 16);   // no clock: 1980-01-01
  return ((uint32_t)(tm.tm_year - 80) << 25) | ((uint32_t)(tm.tm_mon + 1) << 21) |
         ((uint32_t)tm.tm_mday << 16) | ((uint32_t)tm.tm_hour << 11) |
         ((uint32_t)tm.tm_min << 5) | ((uint32_t)tm.tm_sec >> 1);
}

static File   logFile;
static char   path[48] = "";    // fits "/drive_2026-09-25_14-32_9_3fa81c.csv.nge"
static bool   pathDated = false; // path is a dated name, not a fallback number
static uint32_t driveOpenMs = 0; // millis() when the CSV was created
static bool   active = false;   // the CSV is open and taking rows
static bool   cardOk = false;   // card mounted; a file may not be open yet
static bool   gpxOpenFailed = false;  // stop retrying a hopeless open
static uint32_t lastSampleMs = 0;
static uint32_t lastFlushMs  = 0;

// ---- GPX trip track -------------------------------------------------------
static File     gpxFile;
// Sized like `path`: snprintf would truncate a longer name silently rather
// than fail.
static char     gpxPath[48] = "";
static bool     gpxActive = false;
static uint32_t gpxLastMs = 0;
// Byte offset of the point insertion position, i.e. just past the last
// trackpoint and before the closing tags.
static uint32_t gpxTailPos = 0;

static const char GPX_TAIL[] = "</trkseg></trk></gpx>\n";

// ---- encryption --------------------------------------------------------------
// With a key set (settings.logKey), the CSV, the GPX and the .vbo are written
// as logcrypt.h records - see there, and DECISIONS.md "Logs are to be
// encrypted to a public key". Each file goes through a Sink: plaintext as
// before with no key, otherwise a segment header when the file is opened and
// one sealed record per batch the writer already makes. The sleep log stays
// plain on purpose.
//
// The throwaway secret per segment needs real randomness. The S3's RNG is
// only guaranteed true-random while the radio runs (WiFi or BT), and the AP
// is up from boot but drops while driving (AP_DRIVE_OFF) - just when a GPX
// may open at the first fix. So RNG output is stirred into a pool on every
// pass with the AP up, and each secret is BLAKE2b(pool, fresh RNG output); the
// pool is then ratcheted, so the secret cannot be recomputed from it later.
static uint8_t  entropy[32];
static uint32_t entropyStirs = 0;

static void entropyStir() {
  if (!web_is_up()) return;
  uint8_t m[32 + 16];
  memcpy(m, entropy, 32);
  esp_fill_random(m + 32, 16);
  crypto_blake2b(entropy, sizeof(entropy), m, sizeof(m));
  crypto_wipe(m, sizeof(m));
  entropyStirs++;
}

static void ephSecret(uint8_t out[32]) {
  uint8_t m[32 + 32 + 4];
  memcpy(m, entropy, 32);
  esp_fill_random(m + 32, 32);
  memcpy(m + 64, &entropyStirs, 4);
  crypto_blake2b(out, 32, m, sizeof(m));
  m[0] ^= 0x01;                                    // a different input for the ratchet
  crypto_blake2b(entropy, sizeof(entropy), m, sizeof(m));
  crypto_wipe(m, sizeof(m));
  if (entropyStirs == 0)
    Serial.println(F("[..] log key from the RNG with the radio off - the AP was never up this boot"));
}

struct Sink {
  LogcryptSeg seg;
  bool        enc = false;
};
static Sink csvSink, gpxSink, vboSink;
// One sealed record at a time, for every writer: they all run on the logger
// task or under the SD lock. Sized for the largest batch, the GPX's.
static uint8_t sealOut[12288 + LOGCRYPT_OVERHEAD];

// Opening a file, or reopening one: a new segment with a fresh key.
static void sinkBegin(File &f, Sink &s) {
  s.enc = settings.logKey;
  if (!s.enc) return;
  uint8_t eph[32], head[LOGCRYPT_HEADER_LEN];
  ephSecret(eph);
  logcrypt_begin(&s.seg, settings.logPub, eph, head);   // wipes eph
  f.write(head, sizeof(head));
}

static void sinkWrite(File &f, Sink &s, const uint8_t *p, size_t n) {
  if (!s.enc) { f.write(p, n); return; }
  while (n) {
    const size_t k = n < sizeof(sealOut) - LOGCRYPT_OVERHEAD ? n : sizeof(sealOut) - LOGCRYPT_OVERHEAD;
    f.write(sealOut, logcrypt_seal(&s.seg, p, k, sealOut));
    p += k;
    n -= k;
  }
}

static void sinkEnd(Sink &s) {
  if (s.enc) logcrypt_end(&s.seg);
  s.enc = false;
}

// The CSV is staged here and goes to the card on each flush (every
// LOG_FLUSH_INTERVAL_MS) - so that, encrypted, one flush is one record rather
// than one record per row. ~2 KB of rows per 2 s; a full buffer is written
// early.
static char   csvBuf[4096];
static size_t csvLen = 0;

static void csvWriteStaged() {
  if (csvLen) sinkWrite(logFile, csvSink, (const uint8_t *)csvBuf, csvLen);
  csvLen = 0;
}

static void csvCommit() {
  csvWriteStaged();
  logFile.flush();
}

static void csvPut(const char *p, size_t n) {
  if (csvLen + n > sizeof(csvBuf)) csvWriteStaged();
  if (n > sizeof(csvBuf)) n = sizeof(csvBuf);
  memcpy(csvBuf + csvLen, p, n);
  csvLen += n;
}

// ---- idle throttle --------------------------------------------------------
static uint32_t lastMovingMs = 0;

// True once the car has been stationary a while. Deliberately returns false
// when there is no GPS fix: without one we cannot tell standing still from
// driving through a tunnel, and quietly thinning the log would be worse than
// writing a few redundant rows. That also covers sitting in a garage doing
// CAN work with no sky view - full rate, which is what you want there.
static bool logIdle() {
  if (!gps.haveFix) return false;
  if (gps.speedKmh >= LOG_IDLE_SPEED_KMH) {
    lastMovingMs = millis();
    return false;
  }
  return (millis() - lastMovingMs) > LOG_IDLE_AFTER_MS;
}

// Writes the closing tags and then rewinds to before them. The file on the
// card is therefore ALWAYS valid XML, which matters because the power is
// expected to die with the ignition - an unterminated GPX is rejected outright
// by most tools, so "close it properly on shutdown" is not good enough.
// Points accumulate here and go to the card in batches. At 25 Hz a write plus
// seek() per point would be 25 SD operations a second, which stalls the render
// loop; batching keeps every sample with far fewer saves.
// 12 KB, so the batch goes out on GPX_FLUSH_MS (2 s) as intended. At 2 KB it
// filled every ~0.4 s at ~4.5 KB/s, and every save (data, tail, flush) cost
// the card ~13-20 ms: the logger's ~20 ms in most half-seconds on every drive
// of 2026-10-10/11 (logMaxMs), confirmed by a bench test of the card - writes
// into the file buffer ~0 ms, each flush 13 ms settled, 40-130 ms early.
static char     gpxBuf[12288];

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
static uint16_t gpxBufLen = 0;
static uint32_t gpxLastWriteMs = 0;

static void gpxCommit() {
  if (!gpxActive || gpxBufLen == 0) return;
  sinkWrite(gpxFile, gpxSink, (const uint8_t *)gpxBuf, gpxBufLen);
  gpxBufLen = 0;
  // Encrypted, no closing tags: a sealed record cannot be overwritten in
  // place, and the readers add them (the app's cleanGpx, the PC tool).
  if (gpxSink.enc) { gpxFile.flush(); return; }
  // Closing tags then rewind, so the copy on the card is always valid XML -
  // the power dies with the ignition, so there may never be a clean shutdown.
  gpxTailPos = gpxFile.position();
  gpxFile.print(GPX_TAIL);
  gpxFile.seek(gpxTailPos);
  gpxFile.flush();
}

// ---- the session ------------------------------------------------------------
// Every log file's name ends in the session it belongs to, before the
// extension: "trip_2026-10-12_21-19_3fa81c.gpx". A session is one boot or
// wake, so a drive's GPX, CSV and VBO share it, and the app pairs them by it
// alone. Pairing by time took the wrong CSV when the pod restarted a minute
// into a drive (2026-10-12): the GPX from before the restart was nearer.
// Random rather than counted: a counter in NVS restarts with an erased flash,
// and would then repeat ids already on the card.
static char session[7] = "000000";

// ---- choosing the next file number -----------------------------------------
// Both logs used to find a free name by probing /drive_0001, /drive_0002, ...
// with SD_MMC.exists() one at a time from 1. Each probe is a full directory
// search (~29 ms), so with 244 files on the card opening the drive log took
// 7 s - measured - and it ran from loop(), on the rendering core, so the boot
// splash froze for all of it and was then torn down the moment it could
// render, its window long gone. It also got slower with every file written.
//
// Now: one walk of the root directory to find the highest number in use, then
// count up from there. getNextFileName() rather than openNextFile(), because
// openNextFile() OPENS every entry it passes, which is nearly as slow as the
// probing it replaces.
static int highestIndex(const char *prefix, const char *ext) {
  File root = SD_MMC.open("/");
  if (!root) return 0;
  const size_t pl = strlen(prefix);
  int maxN = 0;
  for (String name = root.getNextFileName(); name.length();
       name = root.getNextFileName()) {
    const char *s = name.c_str();
    if (*s == '/') s++;                 // returned as a full path: "/drive_0001.csv"
    if (strncmp(s, prefix, pl) != 0) continue;
    // Count only names that are EXACTLY prefix + digits + "_" + session +
    // ext. Trip files are also written with dated names
    // ("trip_2026-09-25_14-32_3fa81c.gpx"), and atoi() on those would read
    // 2026 - making the next numbered fallback jump to trip_2027.
    const char *d = s + pl;
    const char *e = d;
    while (*e >= '0' && *e <= '9') e++;
    if (e == d || e - d > 4 || *e != '_' || strlen(e) < 8) continue;
    // Encrypted or not (ext + LOGCRYPT_SUFFIX): one sequence of numbers.
    const char *x = e + 8;              // past "_" and the six-character session
    const size_t el = strlen(ext);
    const bool match = strcmp(x, ext) == 0 ||
                       (strncmp(x, ext, el) == 0 && strcmp(x + el, LOGCRYPT_SUFFIX) == 0);
    if (!match) continue;
    const int n = atoi(d);
    if (n > maxN) maxN = n;
  }
  root.close();
  return maxN;
}

// The next number to use, cached in RTC memory. Most opens happen on a wake
// from deep sleep, and this cache survives sleep, so after the first cold boot
// no directory walk is needed at all. 0 = not known yet (power cycle).
RTC_DATA_ATTR static int nextDriveN = 0;
RTC_DATA_ATTR static int nextTripN  = 0;

// First free number at or after *cache, walking the directory once if the
// cache is empty. The exists() check stays, but now it normally runs once: it
// is what keeps this correct if the card was swapped while the pod slept.
// The extension a new log gets: the plain one, plus LOGCRYPT_SUFFIX when a
// key is set (the same test sinkBegin() makes, under the same lock).
static const char *logExt(char *out, size_t n, const char *base) {
  snprintf(out, n, "%s%s", base, settings.logKey ? LOGCRYPT_SUFFIX : "");
  return out;
}

// /<prefix><4 digits>_<session><ext>; numbered by `base`, the plain
// extension, so an encrypted and a plain file never share a number.
static int nextFreeIndex(int *cache, const char *prefix, const char *base,
                         const char *ext, char *out, size_t outLen) {
  if (*cache <= 0) *cache = highestIndex(prefix, base) + 1;
  for (int n = *cache; n <= 9999; n++) {
    snprintf(out, outLen, "/%s%04d_%s%s", prefix, n, session, ext);
    if (SD_MMC.exists(out)) continue;
    *cache = n + 1;
    return n;
  }
  return -1;
}

// Files are named by when they start - "/trip_2026-09-25_14-32_3fa81c.gpx",
// "/drive_2026-09-25_14-32_3fa81c.csv" - because that is how you look for a
// drive afterwards; a bare number means opening files to find the right one.
// The session goes last, before the extension.
//
// LOCAL time, since the name is for a person reading a directory listing (the
// file contents stay in UTC). Year-first so an alphabetical listing is also
// chronological. Only when the clock is trustworthy - by the first CAN frame
// or the first GNSS point it almost always is (RTC at boot, or a fix). A
// second file starting in the same minute - a stall and restart - gets "_2",
// "_3". `t` is the moment to name it after. Returns false to fall back to a
// number.
static bool datedPath(const char *prefix, const char *ext, time_t t,
                      char *out, size_t n) {
  if (!clock_valid()) return false;
  struct tm tm;
  localtime_r(&t, &tm);
  char stamp[20];
  strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M", &tm);
  for (int k = 1; k <= 9; k++) {
    if (k == 1) snprintf(out, n, "/%s%s_%s%s", prefix, stamp, session, ext);
    else        snprintf(out, n, "/%s%s_%d_%s%s", prefix, stamp, k, session, ext);
    if (!SD_MMC.exists(out)) return true;
  }
  return false;
}

static void gpxOpen() {
  char ext[12];
  logExt(ext, sizeof(ext), ".gpx");
  const bool named = datedPath("trip_", ext, time(nullptr), gpxPath, sizeof(gpxPath)) ||
                     nextFreeIndex(&nextTripN, "trip_", ".gpx", ext,
                                   gpxPath, sizeof(gpxPath)) > 0;
  if (named && (gpxFile = SD_MMC.open(gpxPath, FILE_WRITE))) {

    char stamp[24] = "";
    if (clock_valid()) {
      const time_t t = time(nullptr);
      struct tm tm;
      gmtime_r(&t, &tm);
      strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &tm);
    }
    // The gpxtpx namespace must be declared here or the per-point speed
    // extension is unreadable - viewers skip extensions in unknown namespaces.
    // Into the batch buffer (empty at open) and out through gpxCommit(), so
    // it is sealed like the points when encrypting, and followed by the tail
    // when not.
    sinkBegin(gpxFile, gpxSink);
    const int w = snprintf(gpxBuf, sizeof(gpxBuf),
                           "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                           "<gpx version=\"1.1\" creator=\"i30N-gauge\" "
                           "xmlns=\"http://www.topografix.com/GPX/1/1\" "
                           "xmlns:gpxtpx=\"http://www.garmin.com/xmlschemas/"
                           "TrackPointExtension/v1\">\n<trk><name>%s</name><trkseg>\n",
                           stamp[0] ? stamp : "trip");
    gpxBufLen = w > 0 ? (uint16_t)w : 0;
    gpxActive = true;
    gpxCommit();
    Serial.printf("[OK] trip track %s%s\n", gpxPath, gpxSink.enc ? " (encrypted)" : "");
    return;
  }
  // Give up permanently, exactly as driveOpen() does. Without this, every
  // subsequent loop iteration retried - and a retry walks up to 9999
  // SD_MMC.exists() calls at roughly 0.5-2 ms each on 1-bit SDMMC, i.e. 5-20
  // seconds inside a single iteration. That never reaches
  // esp_task_wdt_reset(), so the 8 s watchdog fires and the board reboots,
  // then does it again on the next fix: a permanent reboot loop, triggered by
  // nothing worse than a full card.
  gpxOpenFailed = true;
  Serial.println(F("[ERR] could not open a GPX trip file - GPX disabled"));
}

// Two jobs, in this order: decide whether the current fix is a trustworthy
// point (rate, quality, plausibility), then - only if there is a card - write
// it.
//
// The card checks used to come FIRST, which silently tied the trip distance to
// the SD card: with no card, a full card or a failed open, every call returned
// before the gates, so the trip read 0.0 km for the whole drive. No card is the
// normal bench configuration. Deciding what counts as a real point has nothing
// to do with whether it can be stored.
static void gpxTask() {
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
  const uint32_t interval = logIdle() ? GPX_IDLE_POINT_MS : GPX_POINT_MS;
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
  // Coming out of the Komorany tunnel the track sprayed across the map in
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

  if (!cardOk) return;                 // no card
  if (settings.logLevel < LOG_TRIPS) return;   // logging off: count, don't write
  if (gpxOpenFailed) return;           // see gpxOpen(): do not retry forever
  if (!gpxActive) gpxOpen();           // lazily, on the first accepted point
  if (!gpxActive) return;

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
  if (gapSinceLast) {
    const int room = (int)sizeof(gpxBuf) - (int)gpxBufLen;
    const int wrote = snprintf(gpxBuf + gpxBufLen, room > 0 ? room : 0,
                               "</trkseg><trkseg>\n");
    if (wrote > 0 && wrote < room) gpxBufLen += (uint16_t)wrote;
    else {
      // Full: drain, then the break goes first into the empty buffer - not
      // straight to the file, which would bypass the encryption.
      gpxCommit();
      gpxBufLen = (uint16_t)strlcpy(gpxBuf, "</trkseg><trkseg>\n", sizeof(gpxBuf));
    }
  }

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

  const int room = (int)sizeof(gpxBuf) - (int)gpxBufLen;
  const int wrote = snprintf(gpxBuf + gpxBufLen, room > 0 ? room : 0,
      "<trkpt lat=\"%s\" lon=\"%s\"><ele>%.1f</ele>"
      "<time>%s</time><sat>%u</sat><hdop>%.1f</hdop>"
      "<extensions><gpxtpx:TrackPointExtension>"
      "<gpxtpx:speed>%.2f</gpxtpx:speed>"
      "</gpxtpx:TrackPointExtension></extensions></trkpt>\n",
      latS, lonS, posAlt, stamp, gps.sats, gps.hdop,
      gps.speedKmh / 3.6f);             // m/s, per the TrackPointExtension schema

  if (wrote > 0 && wrote < room) gpxBufLen += (uint16_t)wrote;
  else gpxCommit();                     // didn't fit: drain and drop this one

  // Commit on size or on time, whichever comes first.
  if (gpxBufLen > sizeof(gpxBuf) - 256 || now - gpxLastWriteMs >= GPX_FLUSH_MS) {
    gpxLastWriteMs = now;
    gpxCommit();
  }
}

bool logger_card_ok()         { return cardOk; }

// ---- the SD lock -------------------------------------------------------------
// The logger runs on its own task on core 0 (logger_start), so the screen on
// core 1 never waits for the card - a flush costs 13 ms once settled and up
// to ~460 ms when the card stalls (measured 2026-10-11). Everything else that
// touches the card from core 1 - sleep, the USB share, the phone's file
// pages, the serial dump - takes this lock first, and so waits at most one
// save. Recursive, because the public functions call each other.
static SemaphoreHandle_t sdLock = nullptr;
void logger_sd_lock()   { if (sdLock) xSemaphoreTakeRecursive(sdLock, portMAX_DELAY); }
void logger_sd_unlock() { if (sdLock) xSemaphoreGiveRecursive(sdLock); }

// Set by logger_close() (sleep, reboot): the task stops opening files, so
// nothing reopens one between the close and the power going.
static volatile bool stopped = false;

// Same 1-bit SDMMC wiring as the sniffer sketch: CLK=2, CMD=1, D0=3.
#define SD_CLK 2
#define SD_CMD 1
#define SD_D0  3

void logger_init() {
  if (!sdLock) sdLock = xSemaphoreCreateRecursiveMutex();
  // A new session per boot and per wake: every file it writes carries the id.
  snprintf(session, sizeof(session), "%06lx", (unsigned long)(esp_random() & 0xFFFFFF));
  if (!SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0)) {
    Serial.println(F("[ERR] SD_MMC.setPins failed - no drive log"));
    return;
  }
  if (!SD_MMC.begin("/sdcard", /*1-bit*/true)) {
    Serial.println(F("[..] no SD card - drive logging disabled"));
    return;
  }
  cardOk = true;
  lastMovingMs = millis();   // don't count as idle before we've ever moved
  Serial.println(F("[OK] SD card mounted"));
  // Deliberately NO file yet - see driveOpen().
}

// The card is lent to the PC as a USB drive (usbdisk.cpp): close everything
// and stop touching it. cardOk=false is what every writer here already
// checks, so this one flag stops the CSV, the GPX, the VBO and the sleeplog;
// webapi.cpp checks logger_card_ok() before listing or serving files.
static bool sharedWithPc = false;

static void closeAll();

void logger_suspend() {
  LoggerSdLock lock;
  closeAll();
  cardOk = false;
  sharedWithPc = true;
}

// The card is back. No remount: the PC had it read-only (usbdisk.cpp), so
// FATFS's cached view still holds - and unmounting while a last read from the
// PC might still be in disk_read() is a race not worth having. Logging
// resumes in a new file on its own.
void logger_resume() {
  LoggerSdLock lock;
  sharedWithPc = false;
  cardOk = true;
  gpxOpenFailed = false;
  Serial.println(F("[OK] SD card back"));
}

bool logger_card_shared() { return sharedWithPc; }

// Create the drive log on first use, not at boot.
//
// This matters because of deep sleep: parked, the board wakes every
// SLEEP_POLL_SEC, runs setup(), finds the bus silent and sleeps again. Opening
// a numbered file here at boot meant a brand new drive_NNNN.csv (header row and
// all, so the next boot skipped it and made another) on EVERY wake - about one
// every two seconds while parked, hitting the 9999 ceiling in a few hours and
// leaving the card full of junk. Worse, the free-number scan below then costs
// thousands of exists() calls per wake, stretching each wake and burning the
// current that sleeping was supposed to save.
//
// "First use" means the first row after the first CAN frame - see
// logger_task(). Opening on the first row alone was not lazy enough: every
// bench boot and every parked wake with a silent bus still made a file of a
// few rows, and the card reached 245 of them.
static bool driveOpen() {
  if (!cardOk) return false;
  char ext[12];
  logExt(ext, sizeof(ext), ".csv");
  pathDated = datedPath("drive_", ext, time(nullptr), path, sizeof(path));
  driveOpenMs = millis();
  const bool named = pathDated ||
                     nextFreeIndex(&nextDriveN, "drive_", ".csv", ext,
                                   path, sizeof(path)) > 0;
  if (!named || !(logFile = SD_MMC.open(path, FILE_WRITE))) {
    Serial.println(F("[ERR] could not open a drive log file"));
    cardOk = false;            // don't retry every loop once it's hopeless
    return false;
  }
  static const char HEADER[] = "utc,ms,oilC,oilRaw,coolantC,boostBar,"
                               "gpsFix,sats,speedKmh,lat,lon,gx,gy,gz,"
                               "isoOk,isoAborts,rxOverflows,busOff,busOffs,"
                               "t0bar,t1bar,t2bar,t3bar,t0c,t1c,t2c,t3c,tpmsRaw,"
                               "oilRawPayload,apUp,obdKmh,coolantPid05C,"
                               "absLoadPct,baroKpa,pedalRaw,rpm,timingDeg,iatC,gear,"
                               "logMaxMs,loopMaxMs\r\n";
  sinkBegin(logFile, csvSink);
  csvLen = 0;
  csvPut(HEADER, sizeof(HEADER) - 1);
  csvCommit();
  active = true;
  Serial.printf("[OK] drive log %s%s\n", path, csvSink.enc ? " (encrypted)" : "");
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
  char dated[sizeof(path)];
  // The extension the open file already has - not logExt(): a key change
  // closes every file before it applies, so the two agree, but the name must
  // follow the contents whatever happens.
  if (!datedPath("drive_", csvSink.enc ? ".csv" LOGCRYPT_SUFFIX : ".csv", start, dated, sizeof(dated))) return;
  csvCommit();
  logFile.close();
  sinkEnd(csvSink);
  const bool renamed = SD_MMC.rename(path, dated);
  if (renamed) strlcpy(path, dated, sizeof(path));
  logFile = SD_MMC.open(path, FILE_APPEND);
  if (!logFile) {
    Serial.printf("[ERR] could not reopen drive log %s - logging stopped\n", path);
    active = false;
    cardOk = false;            // same give-up rule as driveOpen()
    return;
  }
  sinkBegin(logFile, csvSink);  // reopened: a new segment, a fresh key
  if (renamed) Serial.printf("[OK] drive log renamed to %s\n", path);
  else         Serial.printf("[ERR] drive log rename failed, keeping %s\n", path);
}

// ---- VBO track session ------------------------------------------------------
// A Racelogic .vbo per track session, for Circuit Tools (free) and other
// motorsport analysers, which plot channels against distance and split laps -
// things neither the GPX nor the CSV can feed them. Only in track mode: the
// file opens once the lap gate is set AND its direction learned (so the
// start/finish line can go into the header), and closes on STOP or sleep.
// One row per GNSS fix, 25 Hz, ~330 bytes: ~30 MB an hour.
//
// Format per Racelogic's .VBO page and real VBOX files (researched
// 2026-10-05): CRLF lines; lat/long in TOTAL arc-minutes with longitude
// positive WEST (calc_vbo_minutes); time HHMMSS.SS UTC; velocity km/h;
// extra channels named in [header], unit in [channel units] (extras only, as
// VBOX HD2 files do), underscored in [column names], values in exponent form.
// ⚠ Not yet opened in Circuit Tools - do that before trusting a session.
static File     vboFile;
static char     vboPath[48] = "";
static bool     vboActive = false, vboFailed = false;
static char     vboBuf[4096];
static uint16_t vboLen = 0;
static uint32_t vboLastWriteMs = 0, vboLastSeq = 0, vboLastFixMs = UINT32_MAX;
RTC_DATA_ATTR static int nextTrackN = 0;

static float gLateral = 0, gLongitudinal = 0;
void logger_note_g(float lateral, float longitudinal) {
  gLateral = lateral;
  gLongitudinal = longitudinal;
}
static bool braking = false;
void logger_note_brake(bool b) { braking = b; }

static volatile float rawGx = 0, rawGy = 0, rawGz = 0;
static volatile bool  haveRawG = false;
void logger_note_raw_g(float gx, float gy, float gz) {
  rawGx = gx; rawGy = gy; rawGz = gz;
  haveRawG = true;
}

// Gear from rpm / OBD speed (calc_gear): 1..8, 0 = none (shifting, clutch
// slip, neutral, crawling), -1 = rpm or speed not current.
static int currentGear() {
  static const float ratios[] = GEAR_RPM_PER_KMH;
  if (!obd_rpm_fresh() || !obd_speed_fresh()) return -1;
  return calc_gear((float)obd.rpm, (float)obd.speedKmh, ratios,
                   sizeof(ratios) / sizeof(ratios[0]), GEAR_TOL, GEAR_MIN_KMH);
}

static void vboCommit() {
  if (vboLen) { sinkWrite(vboFile, vboSink, (const uint8_t *)vboBuf, vboLen); vboLen = 0; }
  vboFile.flush();
}

// The header goes through the batch buffer too, so it is sealed when
// encrypting; a buffer that fills is committed first.
static void vboPut(const char *s) {
  const size_t n = strlen(s);
  if (vboLen + n > sizeof(vboBuf)) vboCommit();
  if (n > sizeof(vboBuf)) return;
  memcpy(vboBuf + vboLen, s, n);
  vboLen += (uint16_t)n;
}

static void vboPutf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void vboPutf(const char *fmt, ...) {
  char line[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  vboPut(line);
}

static void vboClose() {
  if (!vboActive) return;
  vboCommit();
  vboFile.close();
  sinkEnd(vboSink);
  vboActive = false;
  Serial.printf("[OK] track log %s closed\n", vboPath);
}

static void vboOpen() {
  int32_t gLat, gLon;
  float dx, dy;
  if (!laps_gate(&gLat, &gLon, &dx, &dy)) return;
  char ext[12];
  logExt(ext, sizeof(ext), ".vbo");
  const bool named = datedPath("track_", ext, time(nullptr), vboPath, sizeof(vboPath)) ||
                     nextFreeIndex(&nextTrackN, "track_", ".vbo", ext,
                                   vboPath, sizeof(vboPath)) > 0;
  if (!named || !(vboFile = SD_MMC.open(vboPath, FILE_WRITE))) {
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
  sinkBegin(vboFile, vboSink);
  vboLen = 0;
  vboPutf("File created on %s\r\n\r\n", created);
  vboPut("[header]\r\nsatellites\r\ntime\r\nlatitude\r\nlongitude\r\n"
         "velocity kmh\r\nheading\r\nheight\r\n"
         "Boost Pressure\r\nOil Temperature\r\nCoolant Temperature\r\n"
         "Throttle Pedal\r\nEngine Torque Estimate\r\n"
         "Lateral G IMU\r\nLongitudinal G IMU\r\nEngine RPM\r\n"
         "Ignition Timing\r\nIntake Air Temperature\r\nBrake IMU\r\nLap Number\r\n"
         "Tyre Pressure FL\r\nTyre Pressure FR\r\nTyre Pressure RL\r\nTyre Pressure RR\r\n"
         "Tyre Temperature FL\r\nTyre Temperature FR\r\n"
         "Tyre Temperature RL\r\nTyre Temperature RR\r\nGear\r\n\r\n"
         "[channel units]\r\nbar\r\nC\r\nC\r\n%\r\nNm\r\ng\r\ng\r\nrpm\r\n"
         // A blank unit line would end the section, so the two
         // unitless channels get a word instead.
         "deg\r\nC\r\nflag\r\ncount\r\nbar\r\nbar\r\nbar\r\nbar\r\nC\r\nC\r\nC\r\nC\r\ngear\r\n\r\n"
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
  vboPutf("[laptiming]\r\nStart        %s %s %s %s \xC2\xAC  Start / Finish\r\n\r\n",
          aLonS, aLatS, bLonS, bLatS);
  vboPut("[column names]\r\nsats time lat long velocity heading height "
         "Boost_Pressure Oil_Temperature Coolant_Temperature Throttle_Pedal "
         "Engine_Torque_Estimate Lateral_G_IMU Longitudinal_G_IMU Engine_RPM "
         "Ignition_Timing Intake_Air_Temperature Brake_IMU Lap_Number "
         "Tyre_Pressure_FL Tyre_Pressure_FR Tyre_Pressure_RL Tyre_Pressure_RR "
         "Tyre_Temperature_FL Tyre_Temperature_FR "
         "Tyre_Temperature_RL Tyre_Temperature_RR Gear\r\n\r\n"
         "[data]\r\n");
  vboCommit();
  vboActive = true;
  vboLastWriteMs = millis();
  Serial.printf("[OK] track log %s%s\n", vboPath, vboSink.enc ? " (encrypted)" : "");
}

static void vboTask() {
  LapInfo li;
  laps_info(&li);
  if (li.state < LAP_OUT_LAP) {         // no session, or the line not found yet
    vboClose();
    if (li.state == LAP_IDLE) vboFailed = false;   // a new session may try again
    return;
  }
  if (!vboActive && !vboFailed) vboOpen();
  if (!vboActive) return;

  const uint32_t now = millis();
  GpsFix f;
  // One row per NEW fix: RMC and GGA both publish, so one 40 ms epoch can
  // bump seq twice with the same time - the second would duplicate the row.
  if (gps.haveFix && gps_read_fix(&f) && f.seq != vboLastSeq && f.timeOk &&
      f.fixMs != vboLastFixMs &&
      calc_fix_quality_ok(gps.sats, gps.hdop, GPS_MIN_SATS_LOG, GPS_MAX_HDOP_LOG)) {
    vboLastSeq = f.seq;
    vboLastFixMs = f.fixMs;
    char t[16], lat[16], lon[16];
    calc_vbo_time(t, sizeof(t), f.fixMs);
    calc_vbo_minutes(lat, sizeof(lat), f.lat1e7, false);
    calc_vbo_minutes(lon, sizeof(lon), f.lon1e7, true);
    // Engine channels have no "missing" value in a .vbo; a stale one is
    // written as 0 rather than repeating a frozen reading.
    const float boost  = obd_boost_fresh()   ? obd.boostBar : 0.0f;
    const float oil    = obd_oil_fresh()     ? (float)obd.oilTempC : 0.0f;
    const float cool   = obd_coolant_fresh() ? (float)obd.coolantC : 0.0f;
    const float pedal  = obd_pedal_fresh()
        ? constrain(((int)obd.pedalRaw - PEDAL_RAW_IDLE) * 100.0f /
                    (PEDAL_RAW_FULL - PEDAL_RAW_IDLE), 0.0f, 100.0f) : 0.0f;
    const float torque = obd_absload_fresh() ? TORQUE_NM_FROM_LOAD(obd.absLoadPct) : 0.0f;
    const float rpm    = obd_rpm_fresh()     ? (float)obd.rpm : 0.0f;
    const float timing = obd_timing_fresh()  ? obd.timingDeg : 0.0f;
    const float iat    = obd_iat_fresh()     ? (float)obd.iatC : 0.0f;
    // The lap being driven: 1, 2, ... (0 only on an out-lap after a wake
    // mid-session) - the pod's own
    // split, to check the analyser's against.
    const float lap    = li.state == LAP_TIMING ? (float)(li.laps + 1) : 0.0f;
    const int   gear   = currentGear();
    // Tyres are polled once a minute, so most rows repeat the last value;
    // hot pressures over a session are what matters.
    // Indexed FL, FR, RL, RR, as obd.tyreBar is (ui.h's Wheel).
    float tyreBar[4], tyreC[4];
    for (int w = 0; w < 4; w++) {
      const bool ok = obd_tyre_fresh(w);
      tyreBar[w] = ok ? obd.tyreBar[w] : 0.0f;
      tyreC[w]   = ok ? (float)obd.tyreTempC[w] : 0.0f;
    }
    const int room = (int)sizeof(vboBuf) - (int)vboLen;
    const int wrote = snprintf(vboBuf + vboLen, room > 0 ? room : 0,
        "%03u %s %s %s %07.3f %06.2f %+09.2f %+.6E %+.6E %+.6E %+.6E %+.6E %+.6E %+.6E %+.6E"
        " %+.6E %+.6E %+.6E %+.6E %+.6E %+.6E %+.6E %+.6E %+.6E %+.6E %+.6E %+.6E %+.6E\r\n",
        gps.sats, t, lat, lon, gps.speedKmh, gps.courseDeg, f.altM,
        boost, oil, cool, pedal, torque, gLateral, -gLongitudinal, rpm,
        timing, iat, braking ? 1.0f : 0.0f, lap,
        tyreBar[0], tyreBar[1], tyreBar[2], tyreBar[3],
        tyreC[0], tyreC[1], tyreC[2], tyreC[3],
        (float)(gear > 0 ? gear : 0));   // 0 = none or not current
    if (wrote > 0 && wrote < room) vboLen += (uint16_t)wrote;
    else vboCommit();                   // full: drain, drop this row
  }
  if (vboLen > sizeof(vboBuf) - 512 || now - vboLastWriteMs >= 1000) {
    vboLastWriteMs = now;
    vboCommit();
  }
}

// What logging costs the loop core (asked 2026-10-09: does it slow the
// gauge?). The longest logger_task() call and the longest whole loop() pass
// since the previous CSV row, written as logMaxMs / loopMaxMs. A slow pass
// that lines up with a slow logger call is the SD card stalling the screen;
// a frame at ~60 fps is ~16 ms. Two micros() a pass - always on.
static uint32_t logMaxUs = 0, loopMaxUs = 0;
void logger_note_loop_us(uint32_t us) { if (us > loopMaxUs) loopMaxUs = us; }

// One CSV row, built in RAM and written to the card in one go. It used to be
// ~50 print()/printf() calls straight to the File, each a trip through the
// VFS and FAT layers: logMaxMs showed ~20 ms in most half-seconds on every
// drive of 2026-10-10 - a dropped frame twice a second. The GPX and .vbo
// writers already buffered; this is the same. Same print/printf as File, so
// the row code reads as before. ~650 bytes at most (the two hex payloads).
struct RowBuf {
  char   b[1024];
  size_t n = 0;
  void printf(const char *fmt, ...) __attribute__((format(printf, 2, 3))) {
    if (n >= sizeof(b)) return;
    va_list ap;
    va_start(ap, fmt);
    const int w = vsnprintf(b + n, sizeof(b) - n, fmt, ap);
    va_end(ap);
    if (w > 0) n = n + w < sizeof(b) ? n + w : sizeof(b) - 1;   // truncate, never overrun
  }
  void print(const char *s) { printf("%s", s); }
  void print(char c)        { printf("%c", c); }
};

static void loggerTask();

// The logger's own task, on core 0 at priority 1: below CAN (5) and GPS (4),
// which preempt it the moment they have work, and the time a save costs is
// mostly the card working, not this core. A pass every LOGGER_PASS_MS, the
// pace loop() used to give it; the delay also lets core 0's idle task run.
static void loggerTaskLoop(void *) {
  for (;;) {
    {
      LoggerSdLock lock;
      if (!stopped) {
        const uint32_t t0 = micros();
        loggerTask();
        const uint32_t us = micros() - t0;
        if (us > logMaxUs) logMaxUs = us;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(LOGGER_PASS_MS));
  }
}

void logger_start() {
  static bool started = false;
  if (started || !sdLock) return;
  started = true;
  xTaskCreatePinnedToCore(loggerTaskLoop, "logger", 8192, NULL, 1, NULL, 0);
}

static void loggerTask() {
  // A level change from the phone closes every open file, so it applies at
  // once rather than at the next drive; whatever the new level allows opens
  // again as a new file.
  static uint8_t lastLevel = settings.logLevel;
  if (settings.logLevel != lastLevel) {
    Serial.printf("[..] log level %u -> %u\n", lastLevel, settings.logLevel);
    lastLevel = settings.logLevel;
    closeAll();
  }
  // Same for the encryption key: set, changed or removed, the open files
  // end and the next ones start under the new key.
  static uint32_t lastKeyGen = settings_log_key_gen;
  if (settings_log_key_gen != lastKeyGen) {
    lastKeyGen = settings_log_key_gen;
    Serial.printf("[..] log encryption %s\n", settings.logKey ? "key set" : "off");
    closeAll();
  }
  entropyStir();

  gpxTask();                 // its own cadence, independent of the CSV
  if (cardOk && settings.logLevel >= LOG_TRIPS)
    vboTask();               // per GNSS fix, like the GPX - not the CSV's rate

  if (!cardOk) return;
  if (settings.logLevel < LOG_NORMAL) return;          // no drive CSV
  const bool full = settings.logLevel >= LOG_FULL;     // the raw hex columns
  const uint32_t now = millis();
  if (now - lastSampleMs < (logIdle() ? LOG_IDLE_INTERVAL_MS : LOG_INTERVAL_MS)) return;
  lastSampleMs = now;
  if (!active) {
    // No file until the car is on, which everywhere else in the firmware
    // means "a CAN frame has been seen". Not a GNSS fix: a parked car near a
    // window has one too, and this is a car gauge, not a walk logger.
    if (obd.lastFrameMs == 0) return;
    if (!driveOpen()) return;
  } else if (!pathDated && clock_valid()) {
    driveRenameDated();
    if (!active) return;
  }

  // Real wall-clock once the clock is trustworthy from ANY source - GNSS this
  // boot, the PCF85063 at boot, or a value carried through deep sleep. Blank
  // before that, so rows are never stamped with a bogus 1970 date that looks
  // real in a spreadsheet.
  char utc[24] = "";
  if (clock_valid()) {
    const time_t t = time(nullptr);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(utc, sizeof(utc), "%Y-%m-%dT%H:%M:%SZ", &tm);
  }

  // The loop's latest raw IMU sample (logger_note_raw_g), not a read of our
  // own: the I2C bus is shared with touch and the RTC on core 1, and this
  // runs on core 0.
  const float gx = rawGx, gy = rawGy, gz = rawGz;
  const bool haveG = haveRawG;
  // All THREE axes, raw from the IMU - not the projected values the g-meter
  // shows. The pod is mounted at ~45 deg, so no single raw axis is lateral or
  // longitudinal; keeping the raw vector plus GPS speed is what lets the
  // projection (and which axis is which) be worked out offline. gz used to be
  // read and thrown away, which made that impossible.

  // Empty field rather than 0 when a value isn't available - a logged 0 is
  // indistinguishable from a real reading during analysis. "Available" means
  // CURRENT (obd_*_fresh), not "arrived once": after a dropout the raw flags
  // stay true, and the CSV repeated the last value in every row - a flat line
  // that looks exactly like a steady reading.
  static RowBuf row;            // static: 1 KB is too much for the loop stack
  row.n = 0;
  row.printf("%s,%lu,", utc, (unsigned long)now);
  if (obd_oil_fresh())     row.printf("%d,%u,", obd.oilTempC, obd.oilRaw);
  else                     row.print(",,");
  if (obd_coolant_fresh()) row.printf("%d,", obd.coolantC);
  else                     row.print(",");
  if (obd_boost_fresh())   row.printf("%.2f,", obd.boostBar);
  else                     row.print(",");
  // Same snapshot rule as the GPX: lat and lon must come from one fix.
  int32_t rowLat, rowLon;
  const bool rowFix = gps.haveFix && gps_read_position(&rowLat, &rowLon, nullptr);
  row.printf("%d,%u,", rowFix ? 1 : 0, gps.sats);
  if (rowFix) {
    char latS[16], lonS[16];
    calc_fmt_deg(latS, sizeof(latS), rowLat);
    calc_fmt_deg(lonS, sizeof(lonS), rowLon);
    row.printf("%.1f,%s,%s,", gps.speedKmh, latS, lonS);
  } else {
    row.print(",,,");
  }
  // Empty fields, not zeros, when the IMU has never produced a sample - same
  // rule as every other column here.
  if (haveG) row.printf("%.3f,%.3f,%.3f,", gx, gy, gz);
  else       row.print(",,,");
  row.printf("%lu,%lu,%lu,%d,%lu",
                 (unsigned long)obd.isoOk, (unsigned long)obd.isoAborts,
                 (unsigned long)obd.rxOverflows,
                 obd.busOff ? 1 : 0, (unsigned long)obd.busOffs);

  for (int i = 0; i < 4; i++) {
    if (obd_tyre_fresh(i)) row.printf(",%.2f", obd.tyreBar[i]);
    else                   row.print(",");
  }
  for (int i = 0; i < 4; i++) {
    if (obd_tyre_fresh(i)) row.printf(",%d", obd.tyreTempC[i]);
    else                   row.print(",");
  }
  // Raw DID 0xC00B payload as hex. This is the field that will actually
  // solve the wheel layout - space-separated so it stays one CSV column.
  // The two raw payload columns deliberately stay "last payload received",
  // not aged out: they are for decoding the format offline, and the decoded
  // columns beside them already say whether the reading is current.
  // The two raw hex columns are LOG_FULL only; below that they stay as
  // empty columns, so the CSV layout is the same at every level.
  row.print(',');
  for (uint8_t i = 0; full && i < obd.tpmsRawLen; i++) {
    if (i) row.print(' ');
    row.printf("%02X", obd.tpmsRaw[i]);
  }
  // Raw DID 0xE001 payload as hex. The byte we decode as oil temp disagreed
  // with the dashboard by ~20 C on the first drive, and one byte of stored
  // history cannot settle which one is right. With the whole payload, a single
  // cold-start warm-up drive plus a few noted dash readings identifies the
  // correct offset offline. See the note in obd.h.
  row.print(',');
  for (uint8_t i = 0; full && i < obd.oilPayloadLen; i++) {
    if (i) row.print(' ');
    row.printf("%02X", obd.oilPayload[i]);
  }
  // Last, so adding it moved no existing column. With the GPX's per-point
  // sat/hdop it answers whether the AP near the GNSS antenna hurts the fix
  // (see AP_DRIVE_OFF).
  row.printf(",%d", web_is_up() ? 1 : 0);
  // OBD speed, what the brake-light gate sees. Also a speed where GPS has no
  // fix. Appended last for the same reason as apUp.
  if (obd_speed_fresh()) row.printf(",%d", obd.speedKmh);
  else                   row.print(',');
  // The raw coolant sensor (PID 0x05). coolantC above is the dashboard's
  // value (DID 0xE001 byte 25) since 2026-10-03 - see COOLANT_FROM_E001.
  if (obd.havePid05 && (int32_t)(millis() - obd.pid05Ms) < (int32_t)OBD_STALE_MS)
    row.printf(",%d", obd.coolantPid05C);
  else
    row.print(',');
  // Absolute load, for calibrating a torque estimate (one full-throttle pull
  // maps its peak to the engine's rated torque), and the baro boost is zeroed
  // against.
  if (obd_absload_fresh()) row.printf(",%.1f", obd.absLoadPct);
  else                     row.print(',');
  if (obd.haveBaro)        row.printf(",%d", obd.baroKpa);
  else                     row.print(',');
  // Accelerator pedal D, raw (x100/255 = %): 37 with the foot off; the
  // full-throttle value is what is still unknown.
  if (obd_pedal_fresh())   row.printf(",%u", obd.pedalRaw);
  else                     row.print(',');
  if (obd_rpm_fresh())     row.printf(",%d", obd.rpm);
  else                     row.print(',');
  if (obd_timing_fresh())  row.printf(",%.1f", obd.timingDeg);
  else                     row.print(',');
  if (obd_iat_fresh())     row.printf(",%d", obd.iatC);
  else                     row.print(',');
  // 0 = no gear matched (shifting, slip, neutral); empty = rpm/speed stale.
  const int gear = currentGear();
  if (gear >= 0)           row.printf(",%d", gear);
  else                     row.print(',');
  // Since the previous row. The call writing this row is not in it yet - it
  // lands in the next row's figure.
  row.printf(",%.1f,%.1f", logMaxUs / 1000.0f, loopMaxUs / 1000.0f);
  logMaxUs = loopMaxUs = 0;
  // CRLF, as println() wrote it. The +2 is always free: printf() stops at
  // sizeof(b) - 1, and a row is far shorter anyway.
  if (row.n > sizeof(row.b) - 3) row.n = sizeof(row.b) - 3;
  row.b[row.n++] = '\r';
  row.b[row.n++] = '\n';
  csvPut(row.b, row.n);

  if (now - lastFlushMs >= LOG_FLUSH_INTERVAL_MS) {
    lastFlushMs = now;
    csvCommit();   // bounds data loss when the ignition cuts power
  }
}

void logger_note_sleep(const char *decision, int wakeCause, int canRx,
                       unsigned long fruitless) {
  LoggerSdLock lock;
  if (!cardOk) return;

  // Bounded: one line per sleep is tiny, but parked on the backoff timer this
  // appends a few hundred lines a night, forever. Stop growing rather than
  // silently filling the card that the drive logs need.
  if (SD_MMC.exists(SLEEP_LOG_PATH)) {
    File probe = SD_MMC.open(SLEEP_LOG_PATH, FILE_READ);
    if (probe) {
      const size_t sz = probe.size();
      probe.close();
      if (sz > SLEEP_LOG_MAX_BYTES) return;
    }
  }

  File f = SD_MMC.open(SLEEP_LOG_PATH, FILE_APPEND);
  if (!f) return;
  char utc[24] = "no-clock";
  if (clock_valid()) {
    const time_t t = time(nullptr);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(utc, sizeof(utc), "%Y-%m-%dT%H:%M:%SZ", &tm);
  }
  // awakeMs says how long this wake cost us - the number that decides whether
  // parked current is acceptable.
  f.printf("%s awakeMs=%lu wakeCause=%d canRx=%d fruitless=%lu %s\n",
           utc, (unsigned long)millis(), wakeCause, canRx, fruitless, decision);
  f.flush();
  f.close();
}

// Before sleep or a reboot: close everything and stop for good - the task
// must not open a new file between this and the power going.
void logger_close() {
  LoggerSdLock lock;
  closeAll();
  stopped = true;
}

static void closeAll() {
  vboClose();                    // a session resumes into a new file after sleep
  if (gpxActive) {
    gpxCommit();                 // don't lose the batch still in RAM
    if (!gpxSink.enc) {
      // Advance past the tail we keep rewinding over, so the closing tags
      // are the real end of the file, then close.
      gpxFile.seek(gpxTailPos);
      gpxFile.print(GPX_TAIL);
    }
    gpxFile.flush();
    gpxFile.close();
    sinkEnd(gpxSink);
    gpxActive = false;
  }
  if (!active) return;
  csvCommit();
  logFile.close();
  sinkEnd(csvSink);
  active = false;
}

void logger_dump_tail(size_t bytes) {
  LoggerSdLock lock;
  if (!active) {
    Serial.printf("[..] no drive log open (card %s)\n", cardOk ? "mounted" : "NOT mounted");
    return;
  }
  csvCommit();
  if (csvSink.enc) {
    Serial.printf("[..] %s is encrypted - only the phone can read it\n", path);
    return;
  }
  File f = SD_MMC.open(path, FILE_READ);
  if (!f) { Serial.printf("[ERR] cannot read %s\n", path); return; }
  const size_t sz = f.size();
  f.seek(sz > bytes ? sz - bytes : 0);
  Serial.printf("----- tail of %s (%u bytes) -----\n", path, (unsigned)sz);
  uint8_t buf[128];
  size_t n;
  while ((n = f.read(buf, sizeof(buf))) > 0) Serial.write(buf, n);
  Serial.println(F("----- end -----"));
  f.close();
}

