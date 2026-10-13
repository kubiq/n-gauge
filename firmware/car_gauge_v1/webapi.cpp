#include "webapi.h"
#include "config.h"
#include "settings.h"
#include "bsp.h"
#include "obd.h"
#include "gps.h"
#include "rtc.h"            // clock_valid(), rtc_set_from_epoch()
#include "brightness.h"
#include "logger.h"         // logger_card_shared(): the card is on the PC
#include "ota.h"
#include "calc.h"           // calc_parse_range(), for /dl
#include "logcrypt.h"       // logcrypt_fingerprint(), for /api/settings
#include <sys/stat.h>       // stat(), for file sizes without opening them
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>   // captive portal; see the note on dns below
#include <stdlib.h>
#include <time.h>
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"   // fed during long downloads; see handleDownload
#include <math.h>
#include "FS.h"
#include "SD_MMC.h"

static WebServer server(80);
// Captive portal. Every DNS query the phone makes is answered with our own IP,
// so whatever URL its connectivity check requests lands on this server, and
// the catch-all handler below redirects it to the settings page. That is what
// makes the "sign in to network" sheet appear by itself on connecting -
// worth having because the pod's buttons are unreachable and a 10-character
// random passphrase is not something anyone wants to type into a URL bar too.
static DNSServer dns;
static bool up = false;
static String ipText;

// Clamp helper - the phone can post anything, including nothing at all.
static int argInt(const char *name, int cur, int lo, int hi) {
  if (!server.hasArg(name)) return cur;
  const String v = server.arg(name);
  if (v.length() == 0) return cur;
  long n = v.toInt();
  if (n < lo) n = lo;
  if (n > hi) n = hi;
  return (int)n;
}

// The network name is the one user-typed string put back into a page or the
// JSON, and it may hold any printable character: a " or \ unescaped made
// /api/settings invalid JSON, and a ' cut the page's value='...' short
// (found 2026-10-09). Each returns a static buffer - use it before the next
// call. 32 characters at most, so the worst case (6 per char) fits.
static const char *jsonEscape(const char *s) {
  static char out[6 * 32 + 1];
  size_t o = 0;
  for (; *s && o + 6 < sizeof(out); s++) {
    const unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
    else if (c < 0x20)         o += snprintf(out + o, sizeof(out) - o, "\\u%04x", c);
    else                       out[o++] = (char)c;
  }
  out[o] = '\0';
  return out;
}

static const char *htmlEscape(const char *s) {
  static char out[6 * 32 + 1];
  size_t o = 0;
  for (; *s && o + 6 < sizeof(out); s++) {
    const char *e = nullptr;
    switch (*s) {
      case '&':  e = "&amp;";  break;
      case '<':  e = "&lt;";   break;
      case '>':  e = "&gt;";   break;
      case '"':  e = "&quot;"; break;
      case '\'': e = "&#39;";  break;
    }
    if (e) while (*e) out[o++] = *e++;
    else   out[o++] = *s;
  }
  out[o] = '\0';
  return out;
}

static void handleSettingsJson() {
  char b[768];
  size_t n = strlcpy(b, "{", sizeof(b));
  // The table settings, by their JSON names...
  for (size_t i = 0; i < settings_table_n && n < sizeof(b); i++) {
    const SettingDef &d = settings_table[i];
    const float v = setting_get(d);
    if (d.type == SET_BOOL)     n += snprintf(b + n, sizeof(b) - n, "\"%s\":%s,", d.json, v ? "true" : "false");
    else if (d.type == SET_F32) n += snprintf(b + n, sizeof(b) - n, "\"%s\":%.2f,", d.json, v);
    else                        n += snprintf(b + n, sizeof(b) - n, "\"%s\":%ld,", d.json, lroundf(v));
  }
  // ...then the rest. logKey is the fingerprint of the public key the logs
  // are encrypted to, "" when they are not - for the app to check it is
  // its own key (logcrypt_fingerprint; the key itself is public anyway).
  char fp[13] = "";
  if (settings.logKey) logcrypt_fingerprint(fp, settings.logPub);
  if (n < sizeof(b))
    n += snprintf(b + n, sizeof(b) - n,
                  "\"night\":%s,\"apSsid\":\"%s\",\"tzOffset\":%d,\"tzDst\":%s,\"tz\":\"%s\","
                  "\"logKey\":\"%s\",\"views\":\"%s\"}",
                  brightness_is_night() ? "true" : "false", jsonEscape(settings.apSsid),
                  (int)settings.tzOffset, settings.tzDst ? "true" : "false", settings.tz, fp,
                  settings.views);              // digits only: nothing to escape
  if (n >= sizeof(b)) { server.send(500, "text/plain", "settings JSON too long"); return; }
  server.send(200, "application/json", b);
}

static void handleLiveJson() {
  char utc[24] = "not set", local[24] = "not set";
  // clock_valid(), not gps.timeSet: the clock can equally have come from the
  // RTC at boot, from the settings page, or been carried through deep sleep.
  if (clock_valid()) {
    const time_t now = time(nullptr);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(utc, sizeof(utc), "%Y-%m-%d %H:%M:%S", &tm);
    localtime_r(&now, &tm);            // honours settings.tz via tzset()
    strftime(local, sizeof(local), "%Y-%m-%d %H:%M:%S", &tm);
  }
  char b[1300];
  snprintf(b, sizeof(b),
           "{\"can\":{\"linkUp\":%s,\"haveOil\":%s,\"oilTempC\":%d,"
           "\"haveCoolant\":%s,\"coolantC\":%d,\"haveBoost\":%s,\"boostBar\":%.2f},"
           // Bus health. These counters are the whole reason this endpoint is
           // worth opening at the car: they say whether the ISO-TP transfers
           // are actually succeeding, which nothing on the gauge screens shows.
           "\"iso\":{\"ok\":%lu,\"aborts\":%lu,\"rxOverflows\":%lu,"
           "\"busOff\":%s,\"busOffs\":%lu,\"recoveries\":%lu,\"errPassive\":%lu},"
           // Deliberately no lat/lon. Nothing consumes them: what this endpoint
           // is for is telling whether the receiver WORKS, and haveFix, sats
           // and hdop answer that. The coordinates were the only personal data
           // here, readable by anyone who joins the AP - and where a car is
           // parked is usually where its owner lives. The panel's GPS screen
           // and the logs have the position if it is ever wanted.
           "\"gps\":{\"linkUp\":%s,\"haveFix\":%s,\"sats\":%u,\"speedKmh\":%.1f,"
           "\"hdop\":%.1f},"
           "\"clock\":{\"valid\":%s,\"setFromGps\":%s,\"utc\":\"%s\",\"local\":\"%s\","
           "\"tz\":\"%s\"},"
           // driftSec is the only place the RTC's accuracy is ever visible.
           // Over a few drives it says whether the crystal keeps decent time,
           // and if it does not, CAP_SEL is the first thing to suspect.
           "\"rtc\":{\"present\":%s,\"valid\":%s,\"seeded\":%s,\"driftSec\":%ld},"
           // Which build is running, for the app's Settings: the build time
           // and the first bytes of the image's ELF hash (as the Device page).
           "\"fw\":{\"build\":\"%s\",\"hash\":\"%s\"},"
           "\"uptimeMs\":%lu}",
           obd.linkUp ? "true" : "false",
           // Fresh, not "arrived once" - same answer the screen gives.
           obd_oil_fresh() ? "true" : "false", obd.oilTempC,
           obd_coolant_fresh() ? "true" : "false", obd.coolantC,
           obd_boost_fresh() ? "true" : "false", obd.boostBar,
           (unsigned long)obd.isoOk, (unsigned long)obd.isoAborts,
           (unsigned long)obd.rxOverflows,
           obd.busOff ? "true" : "false", (unsigned long)obd.busOffs,
           (unsigned long)obd.recoveries, (unsigned long)obd.errPassive,
           gps.linkUp ? "true" : "false",
           gps.haveFix ? "true" : "false", gps.sats, gps.speedKmh,
           gps.hdop,
           clock_valid() ? "true" : "false",
           gps.timeSet ? "true" : "false", utc, local, settings.tz,
           rtc.present ? "true" : "false", rtc.timeValid ? "true" : "false",
           rtc.seeded ? "true" : "false", rtc.driftSec,
           ota_build(), ota_hash(),
           (unsigned long)millis());
  server.send(200, "application/json", b);
}

static void handleClockSet() {
  // The value MUST arrive as a UTC epoch (Date.now()/1000), never a formatted
  // local string. The browser knows local time; the system clock and the RTC
  // both hold UTC. Parsing a local string here would land every timestamp off
  // by the timezone offset - which is exactly the mktime() bug that already
  // put a silent 1-2 hour error into every logged row once the TZ setting
  // existed. An epoch has no timezone to get wrong.
  // Log on entry, unconditionally. Without this, "the button does nothing" is
  // indistinguishable from "the request never arrived" - and those need
  // completely different investigation.
  Serial.printf("[HTTP] POST /api/clock epoch='%s'\n",
                server.hasArg("epoch") ? server.arg("epoch").c_str() : "(absent)");

  if (!server.hasArg("epoch")) {
    server.send(400, "text/plain", "missing epoch");
    return;
  }
  const long long e = atoll(server.arg("epoch").c_str());
  if (!rtc_set_from_epoch(e)) {
    server.send(400, "text/plain", "not a plausible time - ignored");
    return;
  }

  char utc[24];
  const time_t now = time(nullptr);
  struct tm tm;
  gmtime_r(&now, &tm);
  strftime(utc, sizeof(utc), "%Y-%m-%d %H:%M:%S", &tm);

  char msg[128];
  snprintf(msg, sizeof(msg), "Clock set to %s UTC%s", utc,
           rtc.seeded ? " and saved to the onboard RTC."
                      : " (RTC not written - it will not survive a reboot).");
  server.send(200, "text/plain", msg);
}

// ---- firmware update from the phone (see ota.h) ----------------------------
// The upload callback runs once per received chunk. The WebServer reads the
// whole multipart body inside a single handleClient() call - ~30 s for 1.9 MB
// over the AP - so the watchdog is fed per chunk, as handleDownload does.
static bool otaOk = false, otaStarted = false;

static void handleUpdateUpload() {
  HTTPUpload &up = server.upload();
  switch (up.status) {
    case UPLOAD_FILE_START:
      Serial.printf("[..] firmware upload: %s\n", up.filename.c_str());
      otaStarted = true;
      otaOk = ota_begin(0);
      break;
    case UPLOAD_FILE_WRITE:
      if (otaOk) otaOk = ota_write(up.buf, up.currentSize);
      esp_task_wdt_reset();
      break;
    case UPLOAD_FILE_END:
      if (otaOk) otaOk = ota_end();
      Serial.printf("[..] firmware upload %s (%u bytes)\n",
                    otaOk ? "verified" : "FAILED", (unsigned)up.totalSize);
      break;
    case UPLOAD_FILE_ABORTED:
      ota_abort();
      otaOk = false;
      break;
  }
}

static void handleUpdateDone() {
  if (!otaStarted || !otaOk) {
    String msg = String("Update failed: ") + (otaStarted ? ota_error() : "no file") +
                 ". The current firmware keeps running.";
    otaStarted = false;
    server.send(500, "text/plain", msg);
    return;
  }
  server.send(200, "text/plain",
              "Update installed. The gauge restarts now; give it ~10 s, then reconnect. "
              "If the new firmware fails, it rolls back to this one by itself.");
  delay(500);                       // let the reply leave before restarting
  ota_reboot();
}

static void handleSave() {
  // Every table setting the form sent; the rest stay as they are, so each
  // settings page only touches its own fields. Ranges and the rules between
  // settings are applied by setting_set() and settings_save().
  for (size_t i = 0; i < settings_table_n; i++) {
    const SettingDef &d = settings_table[i];
    if (d.type == SET_BOOL) {
      // An unchecked box is not sent at all, so a hidden "<key>Present" field
      // next to it says the box was on the form: then absent means off, not
      // "unchanged".
      if (server.hasArg(String(d.key) + "Present"))
        setting_set(d, server.hasArg(d.key) ? 1 : 0);
      continue;
    }
    if (!server.hasArg(d.key)) continue;
    const String v = server.arg(d.key);
    if (v.length() == 0) continue;
    setting_set(d, v.toFloat());
  }
  // The screen order, from the app: cleaned by the setter, and the next tap
  // already cycles by it.
  if (server.hasArg("views")) settings_set_views(server.arg("views").c_str());
  settings_save();

  // The one setting with immediate hardware effect - day or night value,
  // whichever applies right now.
  brightness_update(true);

  // Capture the g-meter zero from wherever the pod is sitting right now. Read
  // the IMU live rather than reusing the loop's last sample, so the value
  // belongs to the moment the button was pressed.
  if (server.hasArg("gmZero")) {
    float x, y, z;
    bsp_imu_read(&x, &y, &z);
    settings_set_gmeter_zero(x, y, z);
    Serial.printf("[OK] g-meter re-zeroed from phone: x=%.3f y=%.3f z=%.3f\n",
                  x, y, z);
  }

  // tzPresent marks that the clock controls were on the submitted form, so an
  // unchecked DST box can be told apart from a form that never had one.
  if (server.hasArg("tzPresent")) {
    settings_set_tz_offset((int8_t)argInt("tzOff", settings.tzOffset, -12, 14),
                           server.hasArg("tzDst"));
  }

  // The log encryption key, from the phone app: 64 hex digits, or "off".
  // Under the SD lock, so the logger never opens a file on a half-copied
  // key; it closes its files on the change and starts the next ones under it.
  String msg = "Saved.";
  if (server.hasArg("logPub")) {
    LoggerSdLock lock;
    if (!settings_set_log_key(server.arg("logPub").c_str()))
      msg = "Settings saved, but the log key was rejected (64 hex digits, or off).";
  }

  // WiFi identity last: changing either restarts the AP and drops this
  // connection, so everything else must already be saved by then.
  bool apChanged = false;
  if (server.hasArg("apSsid") && server.arg("apSsid").length() > 0) {
    if (settings_set_ap_ssid(server.arg("apSsid").c_str())) apChanged = true;
    else msg = "Settings saved, but the network name was rejected (1-32 chars).";
  }
  if (server.hasArg("apPass") && server.arg("apPass").length() > 0) {
    if (settings_set_ap_pass(server.arg("apPass").c_str())) apChanged = true;
    else msg = "Settings saved, but the WiFi password was rejected (needs 8-32 chars).";
  }
  if (apChanged) {
    server.send(200, "text/plain",
                "Saved. WiFi changed - reconnect, then rescan the QR on the "
                "gauge if you changed the password.");
    delay(300);                         // let the response actually go out
    WiFi.softAP(settings.apSsid, settings.apPass);
    return;
  }
  server.send(200, "text/plain", msg);
}

// ---- SD card browser -------------------------------------------------------
// The whole workflow pain of this project has been: drive, park, pull the
// card, carry it indoors, and only then find out whether the run captured
// what was needed. Serving the card over the AP means pulling logs onto a
// phone while still standing at the car.

// Everything here is flat in the card root, so a name containing a separator
// or ".." can only be an attempt to escape it.
// A whitelist, not a blacklist. The blacklist version was not exploitable,
// but it relied on a check/use mismatch failing closed: String::indexOf() is
// strchr-based and stops at an embedded NUL, while the length check uses
// length() - so "x%00/../secret" passed the separator check, and only survived
// because c_str() truncated at the same NUL before the filesystem saw it. A
// character whitelist over length() bytes does not depend on that, and also
// rejects CR/LF (the name is interpolated into a Content-Disposition header).
// It covers every name this firmware writes: drive_/trip_ files, dated
// (drive_2026-09-25_14-32.csv, 26 chars) or numbered (trip_0012.gpx), and
// sleeplog.txt.
static bool safeName(const String &n) {
  const size_t len = n.length();
  if (len == 0 || len > 40) return false;
  for (size_t i = 0; i < len; i++) {
    const char c = n[i];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    if (!ok) return false;
  }
  return n.indexOf("..") < 0;
}

static const char *mimeFor(const String &n) {
  if (n.endsWith(LOGCRYPT_SUFFIX)) return "application/octet-stream";   // ciphertext
  if (n.endsWith(".csv")) return "text/csv";
  if (n.endsWith(".gpx")) return "application/gpx+xml";
  return "text/plain";
}

#define FILES_PER_PAGE 20
// The app reads every page in a row, so fewer, larger pages; still bounded,
// since each costs one stat per file inside a single loop() pass.
#define FILES_JSON_PER_PAGE 50

// One page of the card root's files, newest first, into names[] (perPage
// long); *nShown says how many. Returns the total file count. Paged because
// the card holds ~300 files, and listing them all opened every one
// (openNextFile) - many seconds inside one loop() pass, so the 8 s watchdog
// rebooted the gauge each time the page loaded (seen in the car 2026-10-03:
// three reboots, one per refresh). Names come from directory entries alone
// (getNextFileName, no open); only the page's files are then looked at for
// their size. Directory order is creation order on this flat FAT root, so
// the end of it is the newest. Caller holds the SD lock.
// Every name on the card, sorted newest first (calc_card_order), then the
// page's slice. Sorted, not taken in directory order: FAT reuses freed
// slots, so after deletions the newest log sat mid-list. The names go into
// one PSRAM table for the sort - a card of logs can be thousands of names,
// and internal RAM is what WiFi runs on.
static constexpr size_t CARD_NAME_MAX = 48;   // safeName() admits up to 40

static int byCardOrder(const void *a, const void *b) {
  return calc_card_order((const char *)a, (const char *)b);
}

static int listCardPage(int page, int perPage, String *names, int *nShown) {
  *nShown = 0;
  int nFiles = 0;
  char *table = nullptr;
  for (int pass = 0; pass < 2; pass++) {
    File root = SD_MMC.open("/");
    if (!root) break;
    int i = 0;
    bool isDir = false;
    for (String n = root.getNextFileName(&isDir); n.length(); n = root.getNextFileName(&isDir)) {
      if (isDir) continue;
      // Pass 0 counts; pass 1 fills the table (as many as were counted).
      if (pass == 1 && i < nFiles)
        strlcpy(table + i * CARD_NAME_MAX, n.c_str() + (n[0] == '/'), CARD_NAME_MAX);
      i++;
      if ((i & 31) == 0) esp_task_wdt_reset();   // bounded by the file count
    }
    root.close();
    if (pass == 0) {
      nFiles = i;
      if (nFiles == 0) return 0;
      table = (char *)heap_caps_malloc(nFiles * CARD_NAME_MAX, MALLOC_CAP_SPIRAM);
      if (!table) return 0;
    } else if (i < nFiles) {
      nFiles = i;                            // a file went between the passes
    }
  }
  if (!table) return 0;
  qsort(table, nFiles, CARD_NAME_MAX, byCardOrder);
  for (int i = page * perPage; i < nFiles && *nShown < perPage; i++)
    names[(*nShown)++] = table + i * CARD_NAME_MAX;
  heap_caps_free(table);
  return nFiles;
}

// stat() rather than open(): VFS open() runs the same stat() first and then
// also allocates a FILE and an fd.
static long cardFileSize(const String &name) {
  struct stat st;
  return stat(("/sdcard/" + name).c_str(), &st) == 0 ? (long)st.st_size : -1;
}

static void handleFiles() {
  // The logger writes from its own task on core 0: take turns with it.
  LoggerSdLock lock;
  // The card can be lent to the PC as a USB drive - then it is not ours to
  // touch at all (two writers corrupt the FAT).
  if (logger_card_shared()) {
    server.send(503, "text/plain", "The SD card is shared with a PC right now - eject it there first.");
    return;
  }
  // Streamed rather than built in a buffer: there can be dozens of logs, and
  // a fixed buffer here is how you get silent truncation.
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");
  server.sendContent(F(
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Gauge files</title><style>"
    "body{font-family:system-ui,sans-serif;background:#0d1116;color:#e7edf3;margin:0;padding:16px}"
    "h2{color:#E8A33D;margin:0 0 4px}p.sub{color:#8a97a3;font-size:13px;margin:0 0 16px}"
    "table{width:100%;border-collapse:collapse}"
    "td,th{border-bottom:1px solid #26313C;padding:10px 6px;text-align:left;font-size:15px}"
    "a{color:#17A9E6;text-decoration:none}"
    "button{background:none;border:1px solid #26313C;color:#E5544B;border-radius:6px;padding:6px 10px}"
    "</style></head><body><h2>Files</h2>"));

  // usedBytes() can walk the whole FAT when the free-count hint is stale (a
  // card pulled from a PC without ejecting) - feed the watchdog after it.
  uint64_t total = SD_MMC.totalBytes(), used = SD_MMC.usedBytes();
  esp_task_wdt_reset();
  char hdr[220];
  snprintf(hdr, sizeof(hdr),
           "<p class='sub'>%llu of %llu MB used. The newest log is still being "
           "written, so its last few seconds may be missing.</p>"
           "<table><tr><th>file</th><th>size</th><th></th></tr>",
           (unsigned long long)(used / 1048576ULL),
           (unsigned long long)(total / 1048576ULL));
  server.sendContent(hdr);

  const int page = server.hasArg("p") ? max(0, (int)server.arg("p").toInt()) : 0;
  String names[FILES_PER_PAGE];
  int nShown = 0;
  const int nFiles = listCardPage(page, FILES_PER_PAGE, names, &nShown);
  for (int k = 0; k < nShown; k++) {
    const long size = cardFileSize(names[k]);
    char row[240];
    snprintf(row, sizeof(row),
             "<tr><td><a href='/dl?f=%s'>%s</a></td><td>%lu KB</td></tr>",
             names[k].c_str(), names[k].c_str(),
             size > 0 ? (unsigned long)(size / 1024) : 0UL);
    server.sendContent(row);
    esp_task_wdt_reset();
  }

  const int pages = (nFiles + FILES_PER_PAGE - 1) / FILES_PER_PAGE;
  char nav[200];
  snprintf(nav, sizeof(nav), "</table><p class='sub'>page %d of %d (%d files) &nbsp;",
           page + 1, pages > 0 ? pages : 1, nFiles);
  server.sendContent(nav);
  if (page > 0) {
    snprintf(nav, sizeof(nav), "<a href='/files?p=%d'>&larr; newer</a> &nbsp;", page - 1);
    server.sendContent(nav);
  }
  if (page + 1 < pages) {
    snprintf(nav, sizeof(nav), "<a href='/files?p=%d'>older &rarr;</a>", page + 1);
    server.sendContent(nav);
  }
  server.sendContent(F(
    "</p><p class='sub'>Read-only. Delete files with the card in a reader."
    "</p><p class='sub'><a href='/device'>&larr; settings</a></p></body></html>"));
  server.sendContent("");    // terminates the chunked response
}

// The card for the phone app's sync: {"files":[{"name","size"}...],"page",
// "pages","total"}, newest first, paged like /files (?p=0, 1, ...). The app
// compares {name, size} with what it already has and fetches what is new or
// grew. Only names /dl will serve are listed - anything else could never be
// fetched, and the app would retry it on every sync.
static void handleFilesJson() {
  LoggerSdLock lock;
  if (logger_card_shared()) {
    server.send(503, "text/plain", "The SD card is shared with a PC right now.");
    return;
  }
  if (!logger_card_ok()) { server.send(503, "text/plain", "No SD card."); return; }
  const int page = server.hasArg("p") ? max(0, (int)server.arg("p").toInt()) : 0;
  String names[FILES_JSON_PER_PAGE];
  int nShown = 0;
  const int nFiles = listCardPage(page, FILES_JSON_PER_PAGE, names, &nShown);
  const int pages = (nFiles + FILES_JSON_PER_PAGE - 1) / FILES_JSON_PER_PAGE;

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  server.sendContent("{\"files\":[");
  bool firstRow = true;
  for (int k = 0; k < nShown; k++) {
    if (!safeName(names[k])) continue;
    const long size = cardFileSize(names[k]);
    if (size < 0) continue;
    char row[96];
    // safeName() admits no character JSON needs escaped.
    snprintf(row, sizeof(row), "%s{\"name\":\"%s\",\"size\":%ld}",
             firstRow ? "" : ",", names[k].c_str(), size);
    server.sendContent(row);
    firstRow = false;
    esp_task_wdt_reset();
  }
  char tail[80];
  snprintf(tail, sizeof(tail), "],\"page\":%d,\"pages\":%d,\"total\":%d}",
           page, pages > 0 ? pages : 1, nFiles);
  server.sendContent(tail);
  server.sendContent("");    // terminates the chunked response
}

// Chunked, watchdog-fed and abortable, rather than server.streamFile().
//
// streamFile() loops over the whole file inside one loop() iteration, and the
// only esp_task_wdt_reset() is at the end of loop(). Two consequences, one an
// attack and one just broken behaviour:
//   - A client that connects and then stops reading (or sends RST) makes each
//     write retry for up to ~10 s, which blows the 8 s watchdog and reboots
//     the board. Repeat it and the gauge is in a reboot loop while driving -
//     no oil warning, no coolant warning - and each reboot also burns a
//     drive_NNNN file number.
//   - Legitimately downloading a long drive fails the same way. A GPX track
//     runs ~4 MB/hour, so the files this endpoint exists for are exactly the
//     ones big enough to trip it.
// Feeding the watchdog here is safe because the loop is bounded three ways:
// end of file, client disconnect, and an absolute deadline. It cannot mask a
// genuine hang.
static void handleDownload() {
  // Held for the whole download: the logger pauses meanwhile (rows of that
  // time are skipped, the files themselves stay consistent).
  LoggerSdLock lock;
  if (logger_card_shared()) {
    server.send(503, "text/plain", "The SD card is shared with a PC right now.");
    return;
  }
  const String n = server.arg("f");
  if (!safeName(n)) { server.send(400, "text/plain", "bad name"); return; }
  File f = SD_MMC.open("/" + n, FILE_READ);
  if (!f || f.isDirectory()) { server.send(404, "text/plain", "not found"); return; }

  // A Range request is how the phone app resumes a download that the pod's
  // WiFi dropped (a 13-17 MB GPX does not always make it in one go, and
  // DL_MAX_MS ends any single response), and how it fetches only the tail of
  // a log that grew since the last sync. See calc_parse_range().
  const uint32_t size = f.size();
  uint32_t first = 0, last = size ? size - 1 : 0;
  const int r = server.hasHeader("Range")
                    ? calc_parse_range(server.header("Range").c_str(), size, &first, &last)
                    : 0;
  if (r < 0) {
    f.close();
    server.sendHeader("Content-Range", "bytes */" + String(size));
    server.send(416, "text/plain", "range not satisfiable");
    return;
  }
  if (r > 0 && !f.seek(first)) {
    f.close();
    server.send(500, "text/plain", "seek failed");
    return;
  }
  uint32_t left = size ? last - first + 1 : 0;

  WiFiClient client = server.client();
  client.setTimeout(DL_CLIENT_TIMEOUT_MS);
  server.sendHeader("Content-Disposition", "attachment; filename=\"" + n + "\"");
  server.sendHeader("Accept-Ranges", "bytes");
  if (r > 0) {
    char cr[48];
    snprintf(cr, sizeof(cr), "bytes %lu-%lu/%lu", (unsigned long)first,
             (unsigned long)last, (unsigned long)size);
    server.sendHeader("Content-Range", cr);
  }
  server.setContentLength(left);
  server.send(r > 0 ? 206 : 200, mimeFor(n), "");

  static uint8_t buf[1024];
  const uint32_t deadline = millis() + DL_MAX_MS;
  while (left && client.connected() && (int32_t)(millis() - deadline) < 0) {
    const size_t got = f.read(buf, left < sizeof(buf) ? left : sizeof(buf));
    if (!got) break;
    if (client.write(buf, got) != got) break;   // stalled or gone: give up
    left -= got;
    esp_task_wdt_reset();
  }
  f.close();
}

// Deliberately NO delete endpoint. A drive log is frequently the only copy of
// a run that is expensive or impossible to repeat - a hot-engine oil-temp
// trace, a track session - and the only thing guarding this server is the AP
// password. A mistap on a phone should not be able to destroy that. Card
// housekeeping is done deliberately, with the card in a reader.

// ---- settings pages --------------------------------------------------------
// Three short pages instead of one long scroll (2026-10-09; the one page had
// reached ~7.4 KB): Gauge (what the screen shows), Car (sleep, logging, trip,
// clock) and Device (WiFi, firmware, files), with a menu at the top. All post
// to /save, which only changes the fields it is sent - so a checkbox's
// "...Present" marker must stay in the same fieldset as the checkbox.

// Shared by every page. Plain text, not a printf format: no %% escapes.
static const char PAGE_HEAD[] =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Gauge settings</title><style>"
    "body{font-family:system-ui,sans-serif;background:#0d1116;color:#e7edf3;margin:0;padding:16px}"
    "h2{color:#E8A33D;margin:0 0 12px}p.sub{color:#8a97a3;margin:0 0 18px;font-size:13px}"
    "label{display:block;margin:14px 0 4px;font-size:14px;color:#b9c4cf}"
    "input,select{width:100%;box-sizing:border-box;background:#161c23;color:#e7edf3;"
    "border:1px solid #26313C;border-radius:8px;padding:12px;font-size:16px}"
    "label.row{display:flex;align-items:center;gap:10px}"
    "input[type=checkbox]{width:auto;flex:0 0 auto;transform:scale(1.3)}"
    "button{width:100%;margin-top:22px;background:#E8A33D;color:#0d1116;border:0;"
    "border-radius:10px;padding:16px;font-size:17px;font-weight:600}"
    // Secondary action, but it must still read as a BUTTON. A dark fill on the
    // dark page looked disabled - the same colour as the input borders - so it
    // was reported as "I don't think I can click it". Bordered in the accent
    // colour with accent text instead.
    "button.sec{background:#161c23;color:#E8A33D;border:2px solid #E8A33D;"
    "margin-top:12px;font-size:15px;padding:12px;cursor:pointer}"
    "button.sec:active{background:#E8A33D;color:#0d1116}"
    "fieldset{border:1px solid #26313C;border-radius:10px;margin:18px 0;padding:8px 14px 14px}"
    "legend{color:#17A9E6;font-size:13px;padding:0 6px}"
    "#msg{margin-top:14px;font-size:14px;color:#4CC38A;min-height:20px}"
    "#cmsg{margin-top:10px;font-size:13px;color:#4CC38A;min-height:17px}"
    "a{color:#17A9E6}"
    "nav{display:flex;gap:8px;margin:0 0 6px}"
    "nav a{flex:1;text-align:center;padding:11px 0;border:1px solid #26313C;"
    "border-radius:10px;text-decoration:none;color:#b9c4cf;font-size:15px}"
    "nav a.on{border-color:#E8A33D;color:#E8A33D}"
    "</style></head><body><h2>i30 N gauge</h2>";

// The settings form's submit, on every page that has one: post it to /save
// and show the answer under the Save button.
static const char SAVE_JS[] =
    "document.getElementById('f').onsubmit=async e=>{e.preventDefault();"
    "const d=new URLSearchParams(new FormData(e.target));"
    "const m=document.getElementById('msg');m.textContent='Saving...';"
    "try{const r=await fetch('/save',{method:'POST',body:d});m.textContent=await r.text();}"
    "catch(err){m.textContent='Saved (connection dropped - expected if you changed the password).';}};";

enum Page : uint8_t { PAGE_GAUGE, PAGE_CAR, PAGE_DEVICE };

// Head, menu, the page's own body (a printf format) and tail, in one heap
// buffer. Heap, not stack, and the length checked: snprintf truncates
// silently, which gives a subtly broken page with no error anywhere.
// Declared printf-like, so the compiler checks each page's arguments against
// its format, as it did when the page was one snprintf.
static void sendPage(Page page, const char *path, const char *body, ...)
    __attribute__((format(printf, 3, 4)));
static void sendPage(Page page, const char *path, const char *body, ...) {
  const size_t cap = 6144;
  char *b = (char *)malloc(cap);
  if (!b) { server.send(500, "text/plain", "out of memory"); return; }
  size_t n = strlcpy(b, PAGE_HEAD, cap);
  if (n < cap)
    n += snprintf(b + n, cap - n,
                  "<nav><a href='/'%s>Gauge</a><a href='/car'%s>Car</a>"
                  "<a href='/device'%s>Device</a></nav>",
                  page == PAGE_GAUGE  ? " class='on'" : "",
                  page == PAGE_CAR    ? " class='on'" : "",
                  page == PAGE_DEVICE ? " class='on'" : "");
  if (n < cap) {
    va_list ap;
    va_start(ap, body);
    n += vsnprintf(b + n, cap - n, body, ap);
    va_end(ap);
  }
  if (n < cap) n += strlcpy(b + n, "</body></html>", cap - n);
  if (n >= cap) Serial.println(F("[ERR] settings page truncated - raise cap"));

  // no-store, or the phone keeps serving a page from before the last reflash.
  // That is invisible and very confusing: a control added in firmware simply
  // is not there, or is there without its handler, and the board looks broken.
  server.sendHeader("Cache-Control", "no-store, must-revalidate");
  Serial.printf("[HTTP] GET %s -> %u bytes (cap %u)\n", path, (unsigned)n, (unsigned)cap);
  server.send(200, "text/html", b);
  free(b);
}

// Gauge: what the screen shows and when it warns.
static void handleGaugePage() {
  sendPage(PAGE_GAUGE, "/",
    "<form id='f'>"
    "<fieldset><legend>Warnings</legend>"
    "<label>Oil over-temp alarm (&deg;C)</label><input name='oilWarn' type='number' min='0' max='200' value='%d'>"
    "<label>Oil ready / warmed-up (&deg;C)</label><input name='oilRdy' type='number' min='0' max='200' value='%d'>"
    "<label>Coolant warning (&deg;C)</label><input name='cool' type='number' min='0' max='200' value='%d'>"
    "</fieldset>"
    "<fieldset><legend>Display</legend>"
    "<label>Brightness (10-255)</label><input name='bright' type='number' min='10' max='255' value='%u'>"
    "<input type='hidden' name='nightOnPresent' value='1'>"
    "<label class='row'><input name='nightOn' type='checkbox' value='1'%s>"
    " Dim at night</label>"
    "<label>Night brightness (10-255)</label><input name='nightBr' type='number' min='10' max='255' value='%u'>"
    "<p class='sub'>Night starts just after sunset, worked out from the clock "
    "and GPS position, so it follows the seasons. Now: %s.</p>"
    "</fieldset>"
    "<fieldset><legend>G-meter</legend>"
    "<label class='row'><input name='gmZero' type='checkbox' value='1'>"
    " Re-zero now (car parked and level-ish)</label>"
    "<p class='sub'>Cancels the dash mounting tilt, so the dot rests in the "
    "centre. Do it with the car stopped; it is stored until you redo it.</p>"
    "</fieldset>"
    "<fieldset><legend>Tyres</legend>"
    "<p class='sub'>A tyre turns red outside these. Spec (cold): front 2.7, "
    "rear 2.4 bar; warm tyres read ~0.3 higher.</p>"
    "<label>Front low (bar)</label><input name='tLowF' type='number' step='0.05' value='%.2f'>"
    "<label>Front high (bar)</label><input name='tHighF' type='number' step='0.05' value='%.2f'>"
    "<label>Rear low (bar)</label><input name='tLowR' type='number' step='0.05' value='%.2f'>"
    "<label>Rear high (bar)</label><input name='tHighR' type='number' step='0.05' value='%.2f'>"
    "</fieldset>"
    "<button type='submit'>Save</button></form>"
    "<div id='msg'></div>"
    "<script>%s</script>",
    settings.oilWarnC, settings.oilReadyC,
    settings.coolantWarnC, settings.brightness,
    settings.nightOn ? " checked" : "", settings.nightBright,
    !clock_valid() ? "no clock yet, so day" : brightness_is_night() ? "night" : "day",
    settings.tpmsLowBar[0], settings.tpmsHighBar[0],
    settings.tpmsLowBar[1], settings.tpmsHighBar[1],
    SAVE_JS);
}

// Car: sleep and wake, what is logged, the trip, the clock.
static void handleCarPage() {
  sendPage(PAGE_CAR, "/car",
    "<form id='f'>"
    "<fieldset><legend>Sleep &amp; wake</legend>"
    "<label>Sleep after the car goes quiet (minutes, 1-60)</label>"
    "<input name='sleepMin' type='number' min='1' max='60' value='%u'>"
    "<label>Wake on motion (mg, 0 = off)</label>"
    "<input name='womMg' type='number' min='0' max='255' value='%u'>"
    "<p class='sub'>Wakes the gauge when the car is opened or got into, before "
    "the engine runs. Lower = more sensitive: 50 fired on keyboard typing on "
    "a desk, 100 ignored it. Default 80.</p>"
    "</fieldset>"
    "<fieldset><legend>Logging</legend>"
    "<label>What goes on the SD card</label>"
    "<select id='logLvl' name='logLvl'>"
    "<option value='0'>Off</option>"
    "<option value='1'>Trips - GPS tracks only</option>"
    "<option value='2'>Normal - tracks + drive data</option>"
    "<option value='3'>Full - + raw diagnostic bytes</option></select>"
    "<p class='sub'>Applies at once and starts new files. Track sessions "
    "(.vbo) are recorded at every level but Off; the sleep log always is. "
    "Full is for checking new values against the car.</p>"
    "</fieldset>"
    "<fieldset><legend>Trip</legend>"
    "<label>New trip after a stop of (minutes)</label>"
    "<input name='tripGap' type='number' min='1' max='720' value='%u'>"
    "<p class='sub'>The trip distance keeps counting across a stop shorter "
    "than this, and starts again after a longer one. It also survives the pod "
    "sleeping, so a fuel stop does not reset it.</p>"
    "</fieldset>"
    "<fieldset><legend>Clock</legend>"
    "<label>Time zone</label>"
    "<select id='tzOff' name='tzOff'></select>"
    "<input type='hidden' name='tzPresent' value='1'>"
    "<label class='row'><input id='tzDst' name='tzDst' type='checkbox' value='1'>"
    " EU summer time (DST)</label>"
    "<p class='sub'>Time comes from GPS (UTC). Czechia is UTC+1 with DST on.</p>"
    "<button type='button' class='sec' id='ct'>Set clock from this phone</button>"
    // Feedback goes HERE, next to the button - not into #msg at the foot of
    // the page. #msg is below the Save button, so from the Clock section it is
    // off-screen: the clock was being set correctly and silently, and the
    // button was reported as doing nothing. A control's confirmation has to be
    // where the control is.
    "<div id='cmsg'></div>"
    "<p class='sub'>Indoors there is no GPS fix, so the clock stays unset and "
    "logs are written undated. This copies your phone's clock and stores it in "
    "the onboard RTC, so it survives sleep and reboots. GPS still overrides it "
    "once you drive.</p>"
    "</fieldset>"
    "<button type='submit'>Save</button></form>"
    "<div id='msg'></div>"
    // Build the 27 offset options client-side rather than in this buffer -
    // as markup they would be ~1 KB of it, for no gain.
    // Attach the clock handler FIRST, before anything that could throw. This
    // is one script block, so an exception in the timezone-select building
    // below would leave every later handler unattached - the button would
    // render perfectly and simply do nothing, with no clue why.
    "<script>"
    "document.getElementById('ct').onclick=async()=>{"
    "const m=document.getElementById('cmsg');m.textContent='Setting clock...';"
    // Date.now() is already UTC-based, so this carries no timezone to get
    // wrong - see the note in handleClockSet().
    "const d=new URLSearchParams({epoch:Math.floor(Date.now()/1000)});"
    "try{const r=await fetch('/api/clock',{method:'POST',body:d});"
    "m.textContent=await r.text();}"
    "catch(err){m.textContent='Failed: '+err;}};"
    "var s=document.getElementById('tzOff');"
    "for(var i=-12;i<=14;i++){var o=document.createElement('option');"
    "o.value=i;o.text='UTC'+(i<0?i:'+'+i);s.add(o);}"
    "s.value='%d';document.getElementById('tzDst').checked=%s;"
    "document.getElementById('logLvl').value='%u';"
    "%s</script>",
    settings.sleepAfterMin, settings.motionWakeMg,
    settings.tripGapMin,
    (int)settings.tzOffset, settings.tzDst ? "true" : "false",
    settings.logLevel,
    SAVE_JS);
}

// Device: the pod's own WiFi, its firmware, and the card.
static const char *logKeyText() {
  static char t[80];
  if (!settings.logKey) return "off - set it up in the phone app.";
  char fp[13];
  logcrypt_fingerprint(fp, settings.logPub);
  snprintf(t, sizeof(t), "<b>on</b>, key %.4s %.4s %.4s", fp, fp + 4, fp + 8);
  return t;
}

static void handleDevicePage() {
  sendPage(PAGE_DEVICE, "/device",
    "<form id='f'>"
    "<fieldset><legend>WiFi</legend>"
    "<label>Network name (1-32 chars)</label>"
    "<input name='apSsid' type='text' maxlength='32' value='%s'>"
    "<label>Password (8-32 chars, blank = leave unchanged)</label>"
    "<input name='apPass' type='text' minlength='8' maxlength='32' placeholder='unchanged'>"
    "<p class='sub'>Changing either drops this connection. The QR code on the "
    "gauge updates to match.</p>"
    "</fieldset>"
    "<button type='submit'>Save</button></form>"
    "<div id='msg'></div>"
    // Its own form: a file upload is multipart, the settings above are not.
    "<form method='POST' action='/update' enctype='multipart/form-data'>"
    "<fieldset><legend>Firmware</legend>"
    "<p class='sub'>Running: built %s (%s).%s</p>"
    "<input type='file' name='fw' accept='.bin' required>"
    "<button type='submit'>Update firmware</button>"
    "<p class='sub'>Pick car_gauge_v1.ino.bin. Takes ~30-60 s, then the gauge "
    "restarts. If the new firmware fails it rolls back to this one by itself. "
    "Or put it on the card as firmware.bin - installed at the next start, "
    "once per file (it stays on the card).</p>"
    // Both found on the first phone update (2026-10-09): the captive-portal
    // pop-up showed the page but its file picker never opened, and Firefox
    // would not load the page at all while Android routed it over mobile data.
    "<p class='sub'>If nothing happens when you pick the file: this window "
    "may be the WiFi sign-in pop-up, which cannot upload. Open "
    "<b>http://%s</b> in Chrome or Firefox instead, with mobile data off "
    "(or tap \"stay connected\" when the phone says the WiFi has no "
    "internet).</p>"
    "</fieldset></form>"
    "<p class='sub'><a href='/files'>drive logs &amp; GPX tracks</a> &middot; "
    "<a href='/api/live'>live data JSON</a></p>"
    // Read-only here: the key is the phone app's, and only it can read the
    // logs - compare this fingerprint with the one the app shows.
    "<p class='sub'>Log encryption: %s</p>"
    "<script>%s</script>",
    htmlEscape(settings.apSsid),
    ota_build(), ota_hash(),
    ota_rolled_back() ? " <b>An update was rolled back</b> - this is the previous firmware." : "",
    ipText.c_str(),                  // the hint under the Firmware form
    logKeyText(),
    SAVE_JS);
}

void web_stop() {
  if (!up) return;
  dns.stop();
  server.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  up = false;
  ipText = "";
  Serial.println(F("[..] settings AP stopped"));
}

void web_start() {
  if (up) return;

  // esp_wifi takes its buffers from internal RAM and is NOT defensive about
  // running out: a failed allocation deep inside ieee80211_hostap_attach
  // surfaces as a null-pointer panic, not a false return. That really happened
  // here - LVGL's 128 KB pool used to sit in internal DRAM, leaving 52 KB, and
  // every long press rebooted the board. WiFi.mode() alone costs ~46 KB.
  // So check first and refuse with a message you can actually act on.
  const size_t freeInternal =
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (freeInternal < AP_MIN_INTERNAL_HEAP) {
    Serial.printf("[ERR] not starting AP: %u B internal RAM free, need %u\n",
                  (unsigned)freeInternal, (unsigned)AP_MIN_INTERNAL_HEAP);
    return;
  }

  // Report associations and DHCP leases, so "the phone never joined" can be
  // told apart from "it joined and then asked nothing".
  WiFi.onEvent([](arduino_event_id_t e, arduino_event_info_t info) {
    (void)info;
    if (e == ARDUINO_EVENT_WIFI_AP_STACONNECTED)         Serial.println(F("[AP] station connected"));
    else if (e == ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED)   Serial.println(F("[AP] lease handed out"));
    else if (e == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) Serial.println(F("[AP] station left"));
  });

  WiFi.mode(WIFI_AP);
  // Before softAP(): puts the AP outside RFC1918 so Android will actually send
  // its connectivity probe. See the note on WIFI_AP_IP in config.h - this one
  // line is the difference between the portal opening and not.
  if (!WiFi.softAPConfig(IPAddress(WIFI_AP_IP), IPAddress(WIFI_AP_IP),
                         IPAddress(WIFI_AP_NETMASK))) {
    Serial.println(F("[ERR] softAPConfig failed"));
  }
  if (!WiFi.softAP(settings.apSsid, settings.apPass)) {
    Serial.println(F("[ERR] softAP failed"));
    WiFi.mode(WIFI_OFF);
    return;
  }
  ipText = WiFi.softAPIP().toString();

  // Register once: WebServer::on() appends to its handler list, so doing this
  // on every toggle would accumulate duplicates and leak.
  static bool routesRegistered = false;
  if (!routesRegistered) {
    // Explicit methods. server.on() without one registers HTTP_ANY, which let
    // every read-only route accept a request body - and WebServer mallocs the
    // whole body based purely on the client's Content-Length, then allocates
    // one 36-byte argument object per '&' in it. Restricting to GET removes
    // the body-reading path from these five entirely, leaving /save as the
    // only sink.
    server.on("/",             HTTP_GET,  []() {
      Serial.println(F("[AP] page requested"));
      handleGaugePage();
    });
    server.on("/car",          HTTP_GET,  handleCarPage);
    server.on("/device",       HTTP_GET,  handleDevicePage);
    server.on("/save",         HTTP_POST, handleSave);
    server.on("/update",       HTTP_POST, handleUpdateDone, handleUpdateUpload);
    server.on("/api/clock",    HTTP_POST, handleClockSet);
    server.on("/api/settings", HTTP_GET,  handleSettingsJson);
    server.on("/api/live",     HTTP_GET,  handleLiveJson);
    server.on("/files",        HTTP_GET,  handleFiles);
    server.on("/api/files",    HTTP_GET,  handleFilesJson);
    server.on("/dl",           HTTP_GET,  handleDownload);
    // WebServer keeps only the request headers it is told to: without this,
    // hasHeader("Range") is always false and every resume restarts at 0.
    static const char *keep[] = {"Range"};
    server.collectHeaders(keep, 1);
    // Anything else - and that means every OS connectivity probe:
    // /generate_204 (Android), /hotspot-detect.html (Apple),
    // /connecttest.txt and /ncsi.txt (Windows) - gets redirected to the
    // settings page. Seeing a redirect instead of the expected 204 or
    // "Success" is precisely what makes a device decide it is behind a
    // captive portal and offer to open it.
    server.onNotFound([]() {
      // Logged because it is the fork in diagnosing a portal that will not
      // open: either the probe requests arrive here (the phone is asking, and
      // it is our answer it dislikes) or nothing arrives at all (the phone
      // never routed the probe to us). Guessing between those two wastes
      // flash cycles.
      Serial.printf("[AP] probe %s %s host=%s\n",
                    server.method() == HTTP_GET ? "GET" : "other",
                    server.uri().c_str(), server.hostHeader().c_str());
      // A real body, not an empty 302. Android's sendHttpProbe() classifies
      // any 2xx/3xx other than 204 as a portal, so the status was never the
      // problem - but it explicitly treats a 200 whose Content-Length is <= 4
      // as FAILED rather than as a portal, and iOS wants content rather than a
      // bare redirect. A 200 carrying a meta-refresh satisfies both, and is
      // what the ESP32 portal projects that are confirmed working on Android
      // actually return.
      const String url = String("http://") + ipText + "/";
      server.sendHeader("Location", url, true);
      server.send(200, "text/html",
                  "<!DOCTYPE html><html><head>"
                  "<meta http-equiv='refresh' content='0;url=" + url + "'>"
                  "</head><body>i30 N gauge settings: "
                  "<a href='" + url + "'>" + url + "</a></body></html>");
    });
    routesRegistered = true;
  }
  server.begin();

  // Two mechanisms, because DNS hijacking alone did not trigger the portal.
  //
  // 1. RFC 8910 (DHCP option 114): the DHCP lease itself carries the portal
  //    URL. This is what current Android and iOS actually act on, and it does
  //    not depend on the phone choosing to resolve names through us - which it
  //    often will not, since it keeps cellular up and may use its own
  //    resolver. Needs IDF >= 5.4.2; this core is 5.5.4.
  // 2. Wildcard DNS, kept as the fallback for anything older that probes by
  //    hostname.
  const bool portalOk = WiFi.AP.enableDhcpCaptivePortal();
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  const bool dnsOk = dns.start(53, "*", WiFi.softAPIP());

  up = true;
  Serial.printf("[OK] settings AP '%s' up at http://%s/  portal:dhcp=%s dns=%s\n",
                settings.apSsid, ipText.c_str(),
                portalOk ? "yes" : "NO", dnsOk ? "yes" : "NO");
}

bool web_is_up()      { return up; }
uint8_t web_clients() { return up ? (uint8_t)WiFi.softAPgetStationNum() : 0; }
const char *web_ip()  { return ipText.c_str(); }

void web_task() {
  if (!up) return;
  // Both are non-blocking polls. DNS first: the portal sheet only appears if
  // the name lookup is answered before the HTTP probe is attempted.
  dns.processNextRequest();
  // The default 5 s socket timeout is more than half the 8 s watchdog budget,
  // so a client that dribbles a request header can reboot the board on its
  // own. Tighten it per connection.
  WiFiClient c = server.client();
  if (c) c.setTimeout(DL_CLIENT_TIMEOUT_MS);
  server.handleClient();
}
