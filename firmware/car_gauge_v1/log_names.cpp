// ============================================================================
// log_names.cpp  —  what a new log file is called. See log_priv.h.
// ============================================================================
#include "log_priv.h"
#include "rtc.h"        // clock_valid(): a dated name only with a trusted clock
#include "settings.h"   // logKey: the encrypted suffix
#include "taskwdt.h"    // taskwdt_feed: a long directory walk
#include "webapi.h"     // web_beacon_update: the last drive changed
#include <esp_random.h>
#include "SD_MMC.h"
#include <time.h>

// ---- the session ------------------------------------------------------------
// Every log file's name ends in the session it belongs to, before the
// extension: "trip_2026-10-12_21-19_3fa81c.gpx". A session is one boot or
// wake, so a drive's GPX, CSV and VBO share it, and the app pairs them by it
// alone. Pairing by time took the wrong CSV when the pod restarted a minute
// into a drive (2026-10-12): the GPX from before the restart was nearer.
// Random rather than counted: a counter in NVS restarts with an erased flash,
// and would then repeat ids already on the card.
static char session[7] = "000000";

static bool driveNoted = false;

void lognames_new_session() {
  snprintf(session, sizeof(session), "%06lx", (unsigned long)(esp_random() & 0xFFFFFF));
  driveNoted = false;
}

// A drive's log opened this session: it is the last drive now - in NVS, once
// per session, and onto the WiFi beacon if the AP is up (web_beacon_update).
void lognames_note_drive() {
  if (driveNoted) return;
  driveNoted = true;
  settings_set_last_drive(session);
  web_beacon_update();
}

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
  int seen = 0;
  for (String name = root.getNextFileName(); name.length();
       name = root.getNextFileName()) {
    if ((++seen & 31) == 0) taskwdt_feed();   // thousands of names on a full card
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

// First free number at or after *cache, walking the directory once if the
// cache is empty. The cache is the caller's, in RTC memory: most opens happen
// on a wake from deep sleep, and it survives sleep, so after the first cold
// boot no directory walk is needed at all (0 = not known yet, a power cycle).
// The exists() check stays, but now it normally runs once: it is what keeps
// this correct if the card was swapped while the pod slept.
//
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
bool lognames_dated(const char *prefix, const char *ext, time_t t,
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

LogName lognames_new(const char *prefix, const char *base, int *nextN,
                     char *out, size_t n) {
  // The extension a new log gets: the plain one, plus LOGCRYPT_SUFFIX when a
  // key is set (the same test logfile_begin() makes, under the same lock).
  char ext[12];
  snprintf(ext, sizeof(ext), "%s%s", base, settings.logKey ? LOGCRYPT_SUFFIX : "");
  if (lognames_dated(prefix, ext, time(nullptr), out, n)) return LOG_NAME_DATED;
  if (nextFreeIndex(nextN, prefix, base, ext, out, n) > 0) return LOG_NAME_NUMBERED;
  return LOG_NAME_NONE;
}
