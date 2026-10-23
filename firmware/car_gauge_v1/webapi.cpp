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
#include "ui.h"             // ui_show_notice(), from loop() only - see postNotice
#include "calc.h"           // calc_parse_range(), the form and multipart parsers
#include "logcrypt.h"       // logcrypt_fingerprint(), for /api/settings
#include "taskwdt.h"        // taskwdt_feed(): the server's task is not watched
#include <sys/stat.h>       // stat(), for file sizes without opening them
#include <dirent.h>         // readdir(): the card's names without a String each
#include <WiFi.h>
#include <esp_wifi.h>       // esp_wifi_set_vendor_ie: the beacon
#include <esp_http_server.h>
#include <DNSServer.h>   // captive portal; see the note on dns below
#include <stdlib.h>
#include <time.h>
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"    // the running partition's size, for /api/live
#include "esp_system.h"     // esp_reset_reason()
#include "esp_task_wdt.h"
#include <math.h>
#include "FS.h"
#include "SD_MMC.h"

// ---- the server --------------------------------------------------------------
// ESP-IDF's esp_http_server, on its own task (TASK_WEB_*), since 2026-10-21.
// The Arduino WebServer before it ran inside loop(), the screen's loop, and
// read a request with a 5 s wait per byte: a client on a weak link - or one
// dribbling on purpose - froze the gauge for as long as it kept sending, and
// past 8 s the watchdog rebooted it. This one reads with per-connection
// timeouts (WEB_RECV/SEND_TIMEOUT_S), serves several connections, and a slow
// one holds up only the web. The server's task is NOT watched by the
// watchdog, for the same reason: a client stuck mid-request must not reboot
// the pod.
//
// The handlers run on that task. Most only read state or the card (under the
// SD lock, as before) and need nothing more. The few that change what loop()
// owns - settings it reads, the screen (LVGL is loop-only), the RTC on the
// I2C bus the touch panel shares - hand that part to loop() (runOnLoop), or
// post it (the notices, the AP restart, the reboot after an update).
static httpd_handle_t httpd = nullptr;
// Captive portal. Every DNS query the phone makes is answered with our own IP,
// so whatever URL its connectivity check requests lands on this server, and
// the catch-all handler below redirects it to the settings page. That is what
// makes the "sign in to network" sheet appear by itself on connecting -
// worth having because the pod's buttons are unreachable and a 10-character
// random passphrase is not something anyone wants to type into a URL bar too.
static DNSServer dns;
static bool up = false;
static String ipText;
static TaskHandle_t loopTask = nullptr;   // for loopStackMin in /api/live

// ---- requests and replies ------------------------------------------------------
static const char *statusLine(int code) {
  switch (code) {
    case 200: return "200 OK";
    case 206: return "206 Partial Content";
    case 400: return "400 Bad Request";
    case 404: return "404 Not Found";
    case 409: return "409 Conflict";
    case 410: return "410 Gone";
    case 413: return "413 Payload Too Large";
    case 416: return "416 Range Not Satisfiable";
    case 503: return "503 Service Unavailable";
    default:  return "500 Internal Server Error";
  }
}

static esp_err_t reply(httpd_req_t *r, int code, const char *type, const char *body, size_t len) {
  httpd_resp_set_status(r, statusLine(code));
  httpd_resp_set_type(r, type);
  return httpd_resp_send(r, body, (ssize_t)len);
}
static esp_err_t replyText(httpd_req_t *r, int code, const char *msg) {
  return reply(r, code, "text/plain", msg, strlen(msg));
}
// A streamed (chunked) reply, a piece at a time; chunkEnd() closes it.
static void chunk(httpd_req_t *r, const char *s, size_t n) { httpd_resp_send_chunk(r, s, (ssize_t)n); }
static void chunk(httpd_req_t *r, const char *s) { chunk(r, s, strlen(s)); }
static void chunkEnd(httpd_req_t *r) { httpd_resp_send_chunk(r, NULL, 0); }

// The request's arguments, as WebServer's hasArg()/arg() gave them: the query
// string, and for a form POST the body too, in one "a=1&b=2" buffer. Static:
// the server runs one handler at a time, on its one task. Bounded - a body
// that does not fit is refused (413) rather than read in full on the
// client's say-so, as WebServer did.
static char argBuf[2048];
static char argVal[160];          // arg()'s answer: use it before the next call

static bool loadArgs(httpd_req_t *r, bool withBody) {
  argBuf[0] = '\0';
  size_t n = 0;
  if (httpd_req_get_url_query_str(r, argBuf, sizeof(argBuf)) == ESP_OK) n = strlen(argBuf);
  if (!withBody || r->content_len == 0) return true;
  if (n + 1 + r->content_len >= sizeof(argBuf)) return false;
  if (n) argBuf[n++] = '&';
  size_t left = r->content_len;
  while (left) {
    const int k = httpd_req_recv(r, argBuf + n, left);
    if (k == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (k <= 0) return false;
    n += (size_t)k;
    left -= (size_t)k;
  }
  argBuf[n] = '\0';
  return true;
}
static bool hasArg(const char *k) { char t[2]; return calc_form_value(argBuf, k, t, sizeof(t)); }
static const char *arg(const char *k) {
  calc_form_value(argBuf, k, argVal, sizeof(argVal));
  return argVal;
}

// Clamp helper - the phone can post anything, including nothing at all.
static int argInt(const char *name, int cur, int lo, int hi) {
  if (!hasArg(name)) return cur;
  const char *v = arg(name);
  if (!*v) return cur;
  long n = atol(v);
  if (n < lo) n = lo;
  if (n > hi) n = hi;
  return (int)n;
}

// ---- what only loop() may do ---------------------------------------------------
// runOnLoop(): `fn` runs on loop() - at web_task(), between two passes, where
// nothing of loop()'s is half done - while the handler waits. As if the
// handler ran there, which it did with WebServer. Not served within
// WEB_LOOP_WAIT_MS, or the server going down meanwhile, and it is withdrawn
// (the handler answers 503); one already running is waited for.
enum { JOB_IDLE, JOB_POSTED, JOB_RUNNING, JOB_DONE };
static portMUX_TYPE jobMux = portMUX_INITIALIZER_UNLOCKED;
static volatile int jobState = JOB_IDLE;
static void (*jobFn)(void *) = nullptr;
static void *jobArg = nullptr;
static volatile bool stopping = false;   // web_stop() under way

static bool runOnLoop(void (*fn)(void *), void *a) {
  taskENTER_CRITICAL(&jobMux);
  jobFn = fn;
  jobArg = a;
  jobState = JOB_POSTED;
  taskEXIT_CRITICAL(&jobMux);
  const uint32_t t0 = millis();
  for (;;) {
    if (jobState == JOB_DONE) { jobState = JOB_IDLE; return true; }
    const bool late = stopping || calc_elapsed(millis(), t0, WEB_LOOP_WAIT_MS);
    taskENTER_CRITICAL(&jobMux);
    const bool withdrawn = late && jobState == JOB_POSTED;
    if (withdrawn) jobState = JOB_IDLE;
    taskEXIT_CRITICAL(&jobMux);
    if (withdrawn) return false;
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

static void serveLoopJob() {               // loop() only
  taskENTER_CRITICAL(&jobMux);
  const bool mine = jobState == JOB_POSTED;
  if (mine) jobState = JOB_RUNNING;
  taskEXIT_CRITICAL(&jobMux);
  if (!mine) return;
  jobFn(jobArg);
  jobState = JOB_DONE;
}

// A notice for the gauge's screen, shown by loop() on its next pass. NULL
// takes it down. Only the latest counts.
static portMUX_TYPE noticeMux = portMUX_INITIALIZER_UNLOCKED;
static char noticeText[64];
static volatile bool noticePending = false, noticeHide = false;

static void postNotice(const char *text) {
  taskENTER_CRITICAL(&noticeMux);
  if (text) strlcpy(noticeText, text, sizeof(noticeText));
  noticeHide = text == nullptr;
  noticePending = true;
  taskEXIT_CRITICAL(&noticeMux);
}

static void showPostedNotice() {           // loop() only
  if (!noticePending) return;
  char t[sizeof(noticeText)];
  taskENTER_CRITICAL(&noticeMux);
  const bool hide = noticeHide;
  memcpy(t, noticeText, sizeof(t));
  noticePending = false;
  taskEXIT_CRITICAL(&noticeMux);
  ui_show_notice(hide ? NULL : t);
}

// Due times for what loop() does after a handler has answered: the AP
// restarting with a new name or password (it drops the very connection the
// answer goes out on), and the restart into a new firmware. 0 = none.
static volatile uint32_t apRestartAt = 0, rebootAt = 0;
static char oldSsid[sizeof(settings.apSsid)], oldPass[sizeof(settings.apPass)];

// ---- JSON and HTML escaping ------------------------------------------------------
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

static esp_err_t handleSettingsJson(httpd_req_t *r) {
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
  if (n >= sizeof(b)) return replyText(r, 500, "settings JSON too long");
  return reply(r, 200, "application/json", b, n);
}

// ---- the gauge's own resources, for the app's System card --------------------

// Why the gauge last started, in a word.
const char *web_reset_text() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "power on";
    case ESP_RST_DEEPSLEEP: return "wake";
    case ESP_RST_SW:        return "restart";      // OTA, settings, deliberate
    case ESP_RST_PANIC:     return "crash";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_BROWNOUT:  return "supply dip";
    default:                return "other";
  }
}

// SD space in KB, cached: usedBytes() can walk the whole FAT when the
// free-cluster hint is stale, so not on every 2 s poll - once a minute at
// most, and under the logger's lock. 0/0 with no card, or while the PC has it.
void web_sd_space_kb(uint32_t *usedKb, uint32_t *totalKb) {
  static uint32_t used = 0, total = 0, atMs = 0;
  static bool have = false;
  if (!logger_card_ok() || logger_card_shared()) { *usedKb = *totalKb = 0; return; }
  if (!have || millis() - atMs >= 60000) {
    LoggerSdLock lock(have ? 0 : SD_LOCK_WEB_MS);   // a known figure does not wait
    if (lock) {
      const uint32_t t0 = millis();
      total = (uint32_t)(SD_MMC.totalBytes() / 1024);
      used  = (uint32_t)(SD_MMC.usedBytes() / 1024);
      // Suspected 2026-10-23 of holding the card for seconds after a sync.
      if (millis() - t0 >= 300)
        Serial.printf("[..] SD space took %lu ms, under the card's lock\n", (unsigned long)(millis() - t0));
      taskwdt_feed();
      atMs = millis();
      have = true;
    }
  }
  *usedKb = used;
  *totalKb = total;
}

static esp_err_t handleLiveJson(httpd_req_t *r) {
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
  uint32_t sdUsed, sdTotal;
  web_sd_space_kb(&sdUsed, &sdTotal);
  char volts[12] = "null";
  if (obd_volts_fresh()) snprintf(volts, sizeof(volts), "%.2f", obd.volts);
  const esp_partition_t *app = esp_ota_get_running_partition();
  const uint32_t appMax = app ? app->size : 0;
  char b[1500];   // ~1.1 KB in practice; too long answers 500, not a cut-off JSON
  const int len = snprintf(b, sizeof(b),
           "{\"can\":{\"linkUp\":%s,\"haveOil\":%s,\"oilTempC\":%d,"
           "\"haveCoolant\":%s,\"coolantC\":%d,\"haveBoost\":%s,\"boostBar\":%.2f,"
           // PID 0x42, the ECU's supply; null while not fresh (2026-10-14).
           "\"volts\":%s},"
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
           // Resources: internal RAM now and at its lowest since boot (WiFi
           // fails without it), PSRAM, SD in KB, the firmware against its
           // partition, the chip's temperature, why it last started.
           // loopStackMin: the least stack loop() has had free since boot, in
           // bytes - the margin under LVGL's drawing, never measured before
           // (2026-10-21). webStackMin: the same for this server's task.
           "\"sys\":{\"heap\":%u,\"heapMin\":%u,\"heapTotal\":%u,\"psram\":%u,\"psramTotal\":%u,"
           "\"sdUsedKb\":%lu,\"sdTotalKb\":%lu,\"app\":%lu,\"appMax\":%lu,"
           "\"tempC\":%.1f,\"reset\":\"%s\",\"loopStackMin\":%u,\"webStackMin\":%u},"
           "\"uptimeMs\":%lu}",
           obd.linkUp ? "true" : "false",
           // Fresh, not "arrived once" - same answer the screen gives.
           obd_oil_fresh() ? "true" : "false", obd.oilTempC,
           obd_coolant_fresh() ? "true" : "false", obd.coolantC,
           obd_boost_fresh() ? "true" : "false", obd.boostBar, volts,
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
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
           (unsigned long)sdUsed, (unsigned long)sdTotal,
           (unsigned long)ESP.getSketchSize(), (unsigned long)appMax,
           temperatureRead(), web_reset_text(),
           loopTask ? (unsigned)uxTaskGetStackHighWaterMark(loopTask) : 0u,
           (unsigned)uxTaskGetStackHighWaterMark(NULL),
           (unsigned long)millis());
  if (len < 0 || len >= (int)sizeof(b)) return replyText(r, 500, "live JSON too long");
  return reply(r, 200, "application/json", b, (size_t)len);
}

// The RTC is on the I2C bus loop() shares with the touch panel: set on loop().
struct ClockJob { long long epoch; bool ok; };
static void clockOnLoop(void *a) {
  ClockJob *j = (ClockJob *)a;
  j->ok = rtc_set_from_epoch(j->epoch);
}

static esp_err_t handleClockSet(httpd_req_t *r) {
  // The value MUST arrive as a UTC epoch (Date.now()/1000), never a formatted
  // local string. The browser knows local time; the system clock and the RTC
  // both hold UTC. Parsing a local string here would land every timestamp off
  // by the timezone offset - which is exactly the mktime() bug that already
  // put a silent 1-2 hour error into every logged row once the TZ setting
  // existed. An epoch has no timezone to get wrong.
  if (!loadArgs(r, true)) return replyText(r, 413, "request too large");
  // Log on entry, unconditionally. Without this, "the button does nothing" is
  // indistinguishable from "the request never arrived" - and those need
  // completely different investigation.
  Serial.printf("[HTTP] POST /api/clock epoch='%s'\n", hasArg("epoch") ? arg("epoch") : "(absent)");

  if (!hasArg("epoch")) return replyText(r, 400, "missing epoch");
  ClockJob job = { atoll(arg("epoch")), false };
  if (!runOnLoop(clockOnLoop, &job)) return replyText(r, 503, "The gauge is busy - try again.");
  if (!job.ok) return replyText(r, 400, "not a plausible time - ignored");

  char utc[24];
  const time_t now = time(nullptr);
  struct tm tm;
  gmtime_r(&now, &tm);
  strftime(utc, sizeof(utc), "%Y-%m-%d %H:%M:%S", &tm);

  char msg[128];
  snprintf(msg, sizeof(msg), "Clock set to %s UTC%s", utc,
           rtc.seeded ? " and saved to the onboard RTC."
                      : " (RTC not written - it will not survive a reboot).");
  return replyText(r, 200, msg);
}

// ---- firmware update from the phone (see ota.h) ----------------------------
// The image streams in: received a piece at a time, each written to flash
// before the next is read - 1.9 MB is far more than RAM to hold. Both senders,
// the Device page's form and the app (and so every app already installed),
// send it as multipart/form-data; calc_multipart_feed() takes the file's
// bytes out of that. A plain octet-stream body is taken as the image itself.
//
// On the gauge's own screen too, as the SD-card install does (ota.cpp): the
// upload takes ~30 s and the gauge would otherwise just look frozen, then
// restart. The notice counts what has arrived, every 256 KB.
static esp_err_t handleUpdate(httpd_req_t *r) {
  char ct[128] = "", boundary[72];
  httpd_req_get_hdr_value_str(r, "Content-Type", ct, sizeof(ct));
  const bool multipart = calc_multipart_boundary(ct, boundary, sizeof(boundary));
  CalcMultipart mp;
  if (multipart && !calc_multipart_begin(&mp, boundary)) return replyText(r, 400, "bad multipart boundary");
  Serial.printf("[..] firmware upload: %u bytes%s\n", (unsigned)r->content_len,
                multipart ? " (multipart)" : "");

  // PSRAM is fine: Update copies each piece into its own internal buffer
  // before it writes flash.
  constexpr size_t BUF_N = 4096;
  uint8_t *buf = (uint8_t *)heap_caps_malloc(BUF_N, MALLOC_CAP_SPIRAM);
  bool ok = buf && ota_begin(0);
  postNotice(ok ? "Updating firmware..." : NULL);
  size_t left = r->content_len, written = 0, shownAt = 0;
  bool ended = !multipart;
  int timeouts = 0;
  while (ok && left && !stopping) {
    const int k = httpd_req_recv(r, (char *)buf, left < BUF_N ? left : BUF_N);
    if (k == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) continue;
    if (k <= 0) { ok = false; break; }
    timeouts = 0;
    left -= (size_t)k;
    if (multipart) {
      ended = calc_multipart_feed(&mp, buf, (size_t)k, [&](const uint8_t *d, size_t n) {
        if (ok) ok = ota_write(d, n);
        written += n;
      });
    } else {
      ok = ota_write(buf, (size_t)k);
      written += (size_t)k;
    }
    if (ok && written - shownAt >= 256 * 1024) {
      shownAt = written;
      char m[48];
      snprintf(m, sizeof(m), "Updating firmware\n%.1f MB received", written / 1048576.0f);
      postNotice(m);
    }
  }
  if (buf) heap_caps_free(buf);
  if (ok && (!ended || left)) ok = false;        // cut short
  if (ok) ok = ota_end();
  else    ota_abort();
  Serial.printf("[..] firmware upload %s (%u bytes)\n", ok ? "verified" : "FAILED", (unsigned)written);

  if (!ok) {
    char msg[160];
    snprintf(msg, sizeof(msg), "Update failed: %s. The current firmware keeps running.",
             *ota_error() ? ota_error() : stopping ? "the gauge is going to sleep" : "the upload was cut short");
    postNotice(NULL);                // back to the gauge; the phone shows why
    return replyText(r, 500, msg);
  }
  postNotice("Update installed\nrestarting...");
  replyText(r, 200,
            "Update installed. The gauge restarts now; give it ~10 s, then reconnect. "
            "If the new firmware fails, it rolls back to this one by itself.");
  const uint32_t at = millis() + 500;              // the answer goes out first
  rebootAt = at ? at : 1;
  return ESP_OK;
}

// /save's changes, applied on loop(): the settings loop() reads, the panel's
// brightness, the IMU (I2C), the time zone (setenv, which localtime() on
// loop() reads), the log key (under the SD lock, as the logger reads it).
struct SaveJob { const char *msg; bool apChanged; };

static void saveOnLoop(void *a) {
  SaveJob *j = (SaveJob *)a;
  // Every table setting the form sent; the rest stay as they are, so each
  // settings page only touches its own fields. Ranges and the rules between
  // settings are applied by setting_set() and settings_save().
  for (size_t i = 0; i < settings_table_n; i++) {
    const SettingDef &d = settings_table[i];
    if (d.type == SET_BOOL) {
      // An unchecked box is not sent at all, so a hidden "<key>Present" field
      // next to it says the box was on the form: then absent means off, not
      // "unchanged".
      char present[24];
      snprintf(present, sizeof(present), "%sPresent", d.key);
      if (hasArg(present)) setting_set(d, hasArg(d.key) ? 1 : 0);
      continue;
    }
    if (!hasArg(d.key)) continue;
    const char *v = arg(d.key);
    if (!*v) continue;
    setting_set(d, (float)atof(v));
  }
  // The screen order, from the app: cleaned by the setter, and the next tap
  // already cycles by it.
  if (hasArg("views")) settings_set_views(arg("views"));
  settings_save();

  // The one setting with immediate hardware effect - day or night value,
  // whichever applies right now.
  brightness_update(true);

  // Capture the g-meter zero from wherever the pod is sitting right now. Read
  // the IMU live rather than reusing the loop's last sample, so the value
  // belongs to the moment the button was pressed.
  if (hasArg("gmZero")) {
    float x, y, z;
    bsp_imu_read(&x, &y, &z);
    settings_set_gmeter_zero(x, y, z);
    Serial.printf("[OK] g-meter re-zeroed from phone: x=%.3f y=%.3f z=%.3f\n",
                  x, y, z);
  }

  // tzPresent marks that the clock controls were on the submitted form, so an
  // unchecked DST box can be told apart from a form that never had one.
  if (hasArg("tzPresent")) {
    settings_set_tz_offset((int8_t)argInt("tzOff", settings.tzOffset, -12, 14),
                           hasArg("tzDst"));
  }

  // The log encryption key, from the phone app: 64 hex digits, or "off".
  // Under the SD lock, so the logger never opens a file on a half-copied
  // key; it closes its files on the change and starts the next ones under it.
  j->msg = "Saved.";
  if (hasArg("logPub")) {
    LoggerSdLock lock(SD_LOCK_WEB_MS);
    if (!lock)
      j->msg = "Settings saved, but not the log key: the SD card is busy - try again.";
    else if (!settings_set_log_key(arg("logPub")))
      j->msg = "Settings saved, but the log key was rejected (64 hex digits, or off).";
    web_beacon_update();                 // keyed by it
  }

  // WiFi identity last: changing either restarts the AP and drops this
  // connection - after the answer has gone out (apRestartAt, web_task()).
  j->apChanged = false;
  memcpy(oldSsid, settings.apSsid, sizeof(oldSsid));
  memcpy(oldPass, settings.apPass, sizeof(oldPass));
  if (hasArg("apSsid") && *arg("apSsid")) {
    if (settings_set_ap_ssid(arg("apSsid"))) j->apChanged = true;
    else j->msg = "Settings saved, but the network name was rejected (1-32 chars).";
  }
  if (hasArg("apPass") && *arg("apPass")) {
    if (settings_set_ap_pass(arg("apPass"))) j->apChanged = true;
    else j->msg = "Settings saved, but the WiFi password was rejected (8-32 plain ASCII characters).";
  }
}

static esp_err_t handleSave(httpd_req_t *r) {
  if (!loadArgs(r, true)) return replyText(r, 413, "request too large");
  SaveJob job = { nullptr, false };
  if (!runOnLoop(saveOnLoop, &job)) return replyText(r, 503, "The gauge is busy - nothing was saved. Try again.");
  if (job.apChanged) {
    replyText(r, 200,
              "Saved. WiFi changed - reconnect, then rescan the QR on the "
              "gauge if you changed the password.");
    const uint32_t at = millis() + 300;              // let the response actually go out
    apRestartAt = at ? at : 1;
    return ESP_OK;
  }
  return replyText(r, 200, job.msg);
}

// The AP with its new name or password, from web_task() once /save has
// answered. Both are checked on the way in, but the driver has the last
// word. Kept only once it takes them: a saved pair it refuses is an AP that
// never comes up again, on any boot - and the AP is the only way in.
static void restartAp() {
  if (!WiFi.softAP(settings.apSsid, settings.apPass)) {
    Serial.println(F("[ERR] softAP refused the new WiFi name/password - keeping the old"));
    memcpy(settings.apSsid, oldSsid, sizeof(oldSsid));
    memcpy(settings.apPass, oldPass, sizeof(oldPass));
    settings_save();
    WiFi.softAP(settings.apSsid, settings.apPass);
  }
}

// ---- SD card browser -------------------------------------------------------
// The whole workflow pain of this project has been: drive, park, pull the
// card, carry it indoors, and only then find out whether the run captured
// what was needed. Serving the card over the AP means pulling logs onto a
// phone while still standing at the car.

// A name from the phone or off the card, safe to open in the card root -
// calc_safe_name() (host-tested). A name from the phone is URL-decoded first
// (arg()), so an embedded "%00" is a NUL here and stops the name short: the
// whitelist then sees what open() would.
static bool safeName(const char *n) { return calc_safe_name(n, strlen(n)); }

static bool endsWith(const char *s, const char *suffix) {
  const size_t a = strlen(s), b = strlen(suffix);
  return a >= b && strcmp(s + a - b, suffix) == 0;
}
static const char *mimeFor(const char *n) {
  if (endsWith(n, LOGCRYPT_SUFFIX)) return "application/octet-stream";   // ciphertext
  if (endsWith(n, ".csv")) return "text/csv";
  if (endsWith(n, ".gpx")) return "application/gpx+xml";
  return "text/plain";
}

#define FILES_PER_PAGE 20
// The app reads every page in a row, so fewer, larger pages; still bounded,
// since each costs one stat per file.
#define FILES_JSON_PER_PAGE 50

// One page of the card root's files, newest first, into out->names (perPage
// long); out->shown says how many. Returns the total file count. Paged because
// the card holds ~300 files, and listing them all opened every one
// (openNextFile) - many seconds inside one loop() pass, so the 8 s watchdog
// rebooted the gauge each time the page loaded (seen in the car 2026-10-03:
// three reboots, one per refresh). Names come from directory entries alone
// (no open); only the page's files are then looked at for their size.
// Every name on the card, sorted newest first (calc_card_order), then the
// page's slice. Sorted, not taken in directory order: FAT reuses freed
// slots, so after deletions the newest log sat mid-list. The names go into
// one PSRAM table for the sort - a card of logs can be thousands of names,
// and internal RAM is what WiFi runs on. Caller holds the SD lock.
// A table slot, NUL included. A name that does not fit is stored empty rather
// than cut: a cut name could pass safeName() and stand for another file.
static constexpr size_t CARD_NAME_MAX = 48;
static_assert(CARD_NAME_MAX > CALC_SAFE_NAME_MAX, "every safe name must fit a slot");

static int byCardOrder(const void *a, const void *b) {
  return calc_card_order((const char *)a, (const char *)b);
}

// ?p= of the card listings: -1 for anything but a page number (a 400). With
// at most this many pages, page * perPage stays far inside an int. An empty
// ?p= is a 400, no ?p= at all page 0 - hence the two lookups.
static constexpr int CARD_MAX_PAGE = 100000;
static int pageArg() {
  return hasArg("p") ? calc_parse_page(arg("p"), CARD_MAX_PAGE) : 0;
}

// One page of the card's names, pointing into the sorted table - which it
// owns, in PSRAM. It used to copy the page into Strings, and the walk made a
// String per name on the card: internal RAM, which WiFi runs on, churned by
// every sync (B16). readdir() hands each name over without allocating.
struct CardPage {
  char       *table = nullptr;
  const char *names[FILES_JSON_PER_PAGE];
  int         shown = 0;
  CardPage() = default;
  CardPage(const CardPage &) = delete;
  CardPage &operator=(const CardPage &) = delete;
  ~CardPage() { if (table) heap_caps_free(table); }
};
static_assert(FILES_PER_PAGE <= FILES_JSON_PER_PAGE, "a CardPage holds either page");

// Returns the file count, or -1 when the card could not be listed - not 0:
// "no files" told the app every pending delete was already done.
static int listCardPage(int page, int perPage, CardPage *out) {
  int nFiles = 0;
  for (int pass = 0; pass < 2; pass++) {
    DIR *dir = opendir("/sdcard");
    if (!dir) return -1;
    int i = 0;
    for (struct dirent *e = readdir(dir); e; e = readdir(dir)) {
      if (e->d_type == DT_DIR) continue;
      // Pass 0 counts; pass 1 fills the table (as many as were counted).
      if (pass == 1 && i < nFiles) {
        char *slot = out->table + i * CARD_NAME_MAX;
        if (strlen(e->d_name) < CARD_NAME_MAX) strcpy(slot, e->d_name);
        else slot[0] = '\0';
      }
      i++;
    }
    closedir(dir);
    if (pass == 0) {
      nFiles = i;
      if (nFiles == 0) return 0;
      out->table = (char *)heap_caps_malloc(nFiles * CARD_NAME_MAX, MALLOC_CAP_SPIRAM);
      if (!out->table) return -1;
    } else if (i < nFiles) {
      nFiles = i;                            // a file went between the passes
    }
  }
  qsort(out->table, nFiles, CARD_NAME_MAX, byCardOrder);
  for (int i = page * perPage; i < nFiles && out->shown < perPage; i++)
    out->names[out->shown++] = out->table + i * CARD_NAME_MAX;
  return nFiles;
}

// stat() rather than open(): VFS open() runs the same stat() first and then
// also allocates a FILE and an fd.
static long cardFileSize(const char *name) {
  char path[16 + CARD_NAME_MAX];
  snprintf(path, sizeof(path), "/sdcard/%s", name);
  struct stat st;
  return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

// The card for a phone request, once `lock` was tried: false, with the 503
// sent, when it is busy (a save on a stalling card holds the lock - see
// LoggerSdLock), lent to the PC (not ours to touch at all - two writers
// corrupt the FAT), or missing. 503 for all three, never 404 or 410: the app
// takes those as "gone" and would drop a file, or a delete it still has
// pending.
static bool cardForRequest(httpd_req_t *r, const LoggerSdLock &lock) {
  if (!lock) { replyText(r, 503, "The SD card is busy - try again in a moment."); return false; }
  if (logger_card_shared()) {
    replyText(r, 503, "The SD card is shared with a PC right now - eject it there first.");
    return false;
  }
  if (!logger_card_ok()) { replyText(r, 503, "No SD card."); return false; }
  return true;
}

static esp_err_t handleFiles(httpd_req_t *r) {
  loadArgs(r, false);
  const int page = pageArg();
  if (page < 0) return replyText(r, 400, "bad page");
  // The logger writes from its own task: take turns with it.
  LoggerSdLock lock(SD_LOCK_WEB_MS);
  if (!cardForRequest(r, lock)) return ESP_OK;
  // Streamed rather than built in a buffer: there can be dozens of logs, and
  // a fixed buffer here is how you get silent truncation.
  httpd_resp_set_type(r, "text/html");
  chunk(r,
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Gauge files</title><style>"
    "body{font-family:system-ui,sans-serif;background:#0d1116;color:#e7edf3;margin:0;padding:16px}"
    "h2{color:#E8A33D;margin:0 0 4px}p.sub{color:#8a97a3;font-size:13px;margin:0 0 16px}"
    "table{width:100%;border-collapse:collapse}"
    "td,th{border-bottom:1px solid #26313C;padding:10px 6px;text-align:left;font-size:15px}"
    "a{color:#17A9E6;text-decoration:none}"
    "button{background:none;border:1px solid #26313C;color:#E5544B;border-radius:6px;padding:6px 10px}"
    "</style></head><body><h2>Files</h2>");

  // The cached figure (web_sd_space_kb, refreshed once a minute), not a fresh
  // usedBytes(): that can walk the whole FAT when the free-count hint is
  // stale (a card pulled from a PC without ejecting), on every page load.
  uint32_t usedKb, totalKb;
  web_sd_space_kb(&usedKb, &totalKb);
  char hdr[220];
  const int hl = snprintf(hdr, sizeof(hdr),
           "<p class='sub'>%lu of %lu MB used. The newest log is still being "
           "written, so its last few seconds may be missing.</p>"
           "<table><tr><th>file</th><th>size</th><th></th></tr>",
           (unsigned long)(usedKb / 1024), (unsigned long)(totalKb / 1024));
  chunk(r, hdr, (size_t)hl);

  CardPage list;
  int nFiles = listCardPage(page, FILES_PER_PAGE, &list);
  if (nFiles < 0) {
    chunk(r, "</table><p class='sub'>The card could not be read.</p>");
    nFiles = 0;
  }
  for (int k = 0; k < list.shown; k++) {
    const char *name = list.names[k];
    // Card names go into HTML and a link: only ones safeName() admits, which
    // carry no markup and are the only ones /dl would serve anyway.
    if (!safeName(name)) continue;
    const long size = cardFileSize(name);
    char row[240];
    const int len = snprintf(row, sizeof(row),
             "<tr><td><a href='/dl?f=%s'>%s</a></td><td>%lu KB</td></tr>",
             name, name, size > 0 ? (unsigned long)(size / 1024) : 0UL);
    chunk(r, row, (size_t)len);
  }

  const int pages = (nFiles + FILES_PER_PAGE - 1) / FILES_PER_PAGE;
  char nav[200];
  snprintf(nav, sizeof(nav), "</table><p class='sub'>page %d of %d (%d files) &nbsp;",
           page + 1, pages > 0 ? pages : 1, nFiles);
  chunk(r, nav);
  if (page > 0) {
    snprintf(nav, sizeof(nav), "<a href='/files?p=%d'>&larr; newer</a> &nbsp;", page - 1);
    chunk(r, nav);
  }
  if (page + 1 < pages) {
    snprintf(nav, sizeof(nav), "<a href='/files?p=%d'>older &rarr;</a>", page + 1);
    chunk(r, nav);
  }
  chunk(r,
    "</p><p class='sub'>Read-only. Delete drives in the phone app, or with the card in a reader."
    "</p><p class='sub'><a href='/device'>&larr; settings</a></p></body></html>");
  chunkEnd(r);
  return ESP_OK;
}

// The card for the phone app's sync: {"files":[{"name","size"}...],"page",
// "pages","total"}, newest first, paged like /files (?p=0, 1, ...). The app
// compares {name, size} with what it already has and fetches what is new or
// grew. Only names /dl will serve are listed - anything else could never be
// fetched, and the app would retry it on every sync.
static esp_err_t handleFilesJson(httpd_req_t *r) {
  loadArgs(r, false);
  const int page = pageArg();
  if (page < 0) return replyText(r, 400, "bad page");
  LoggerSdLock lock(SD_LOCK_WEB_MS);
  if (!cardForRequest(r, lock)) return ESP_OK;
  CardPage list;
  const int nFiles = listCardPage(page, FILES_JSON_PER_PAGE, &list);
  // An error, not an empty card: the app takes a file missing from the list
  // as deleted, and would drop every delete it still has pending.
  if (nFiles < 0) return replyText(r, 500, "The card could not be read.");
  const int pages = (nFiles + FILES_JSON_PER_PAGE - 1) / FILES_JSON_PER_PAGE;

  httpd_resp_set_type(r, "application/json");
  chunk(r, "{\"files\":[");
  bool firstRow = true;
  for (int k = 0; k < list.shown; k++) {
    const char *name = list.names[k];
    if (!safeName(name)) continue;
    const long size = cardFileSize(name);
    if (size < 0) continue;
    char row[96];
    // safeName() admits no character JSON needs escaped.
    const int len = snprintf(row, sizeof(row), "%s{\"name\":\"%s\",\"size\":%ld}",
                             firstRow ? "" : ",", name, size);
    chunk(r, row, (size_t)len);
    firstRow = false;
  }
  char tail[80];
  snprintf(tail, sizeof(tail), "],\"page\":%d,\"pages\":%d,\"total\":%d}",
           page, pages > 0 ? pages : 1, nFiles);
  chunk(r, tail);
  chunkEnd(r);
  return ESP_OK;
}

// A drive's log deleted from the card, from the app's "Delete from the phone
// and the gauge" - otherwise every sync pulls it back. POST, f=<name>.
// Only drive logs (trip_/drive_/track_ ... .gpx/.csv/.vbo, plain or .nge):
// never the settings, the sleep log or a firmware .bin. Not a file the logger
// has open - deleting it would lose the rest of the drive.
// 410 for a file that is already gone, which the app counts as done - not
// 404: an unknown path is answered by the portal (a 200 page), so the app
// takes only "deleted" or 410 as done, and older firmware without this
// handler never looks like a success.
static esp_err_t handleDelete(httpd_req_t *r) {
  if (!loadArgs(r, true)) return replyText(r, 413, "request too large");
  // Busy, shared or no card is "not now", not "already gone": with the card
  // missing, every file looked absent, and a 410 told the app to forget the
  // pending delete.
  LoggerSdLock lock(SD_LOCK_WEB_MS);
  if (!cardForRequest(r, lock)) return ESP_OK;
  char n[CARD_NAME_MAX];
  strlcpy(n, arg("f"), sizeof(n));
  if (!safeName(n) || !calc_is_drive_log(n, LOGCRYPT_SUFFIX))
    return replyText(r, 400, "not a drive log");
  if (logger_is_open(n))
    return replyText(r, 409, "That log is still being written - delete it after the drive.");
  char path[CARD_NAME_MAX + 2];
  snprintf(path, sizeof(path), "/%s", n);
  if (!SD_MMC.exists(path)) return replyText(r, 410, "already gone");
  if (!SD_MMC.remove(path)) return replyText(r, 500, "could not delete it");
  Serial.printf("[OK] deleted %s (app)\n", n);
  return replyText(r, 200, "deleted");
}

// ---- downloads ---------------------------------------------------------------
// On the server's task, so loop() - the screen, the warnings - never waits on
// a transfer (before 2026-10-21 one took up to DL_MAX_MS, two minutes, inside
// loop(); then a task of its own; now the server's own task is that). The SD
// lock is taken per piece, not for the whole transfer, so the logger keeps
// writing during a sync. A client that takes nothing for WEB_SEND_TIMEOUT_S
// is dropped (httpd_send's timeout), a transfer ends at DL_MAX_MS, and
// web_stop() ends a running one (dlAbort) - sleep unmounts the card next.
//
// Headers and body sent raw (httpd_send), with a Content-Length: the server's
// own replies would be chunked, and the app tells a honoured Range from a
// whole file by its size.
static volatile bool dlAbort = false;
static volatile bool dlBusy = false;
bool web_download_active() { return dlBusy; }

static bool sendRaw(httpd_req_t *r, const char *p, size_t n) {
  while (n) {
    const int k = httpd_send(r, p, n);
    if (k <= 0) return false;              // gone, or took nothing in WEB_SEND_TIMEOUT_S
    p += k;
    n -= (size_t)k;
  }
  return true;
}

static esp_err_t handleDownload(httpd_req_t *r) {
  loadArgs(r, false);
  char n[CARD_NAME_MAX];
  strlcpy(n, arg("f"), sizeof(n));
  if (!safeName(n)) return replyText(r, 400, "bad name");
  char path[CARD_NAME_MAX + 2];
  snprintf(path, sizeof(path), "/%s", n);
  File f;
  {
    LoggerSdLock lock(SD_LOCK_WEB_MS);
    if (!cardForRequest(r, lock)) return ESP_OK;
    f = SD_MMC.open(path, FILE_READ);
  }
  if (!f || f.isDirectory()) return replyText(r, 404, "not found");

  // A Range request is how the phone app resumes a download that the pod's
  // WiFi dropped (a 13-17 MB GPX does not always make it in one go, and
  // DL_MAX_MS ends any single response), and how it fetches only the tail of
  // a log that grew since the last sync. See calc_parse_range().
  const uint32_t size = f.size();
  uint32_t first = 0, last = size ? size - 1 : 0;
  char range[64];
  const int rr = httpd_req_get_hdr_value_str(r, "Range", range, sizeof(range)) == ESP_OK
                     ? calc_parse_range(range, size, &first, &last)
                     : 0;
  if (rr < 0) {
    f.close();
    char cr[40];
    snprintf(cr, sizeof(cr), "bytes */%lu", (unsigned long)size);
    httpd_resp_set_hdr(r, "Content-Range", cr);
    return replyText(r, 416, "range not satisfiable");
  }
  if (rr > 0 && !f.seek(first)) {
    f.close();
    return replyText(r, 500, "seek failed");
  }
  uint32_t left = size ? last - first + 1 : 0;

  char head[400], cr[64] = "";
  if (rr > 0)
    snprintf(cr, sizeof(cr), "Content-Range: bytes %lu-%lu/%lu\r\n", (unsigned long)first,
             (unsigned long)last, (unsigned long)size);
  const int hl = snprintf(head, sizeof(head),
      "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\n"
      "Content-Disposition: attachment; filename=\"%s\"\r\nAccept-Ranges: bytes\r\n%s"
      "Connection: close\r\n\r\n",
      statusLine(rr > 0 ? 206 : 200), mimeFor(n), (unsigned long)left, n, cr);

  dlBusy = true;
  const uint32_t t0 = millis(), deadline = t0 + DL_MAX_MS, total = left;
  // Internal RAM: the card reads into it by DMA.
  static uint8_t buf[2048];
  bool sent = sendRaw(r, head, (size_t)hl);
  uint32_t sinceYield = 0;
  while (sent && left && !dlAbort && !calc_reached(millis(), deadline)) {
    size_t got = 0;
    {
      LoggerSdLock lock(SD_LOCK_WEB_MS);
      if (!lock || logger_card_shared()) break;
      got = f.read(buf, left < sizeof(buf) ? left : sizeof(buf));
    }
    if (!got) break;
    if (!sendRaw(r, (const char *)buf, got)) break;      // stalled or gone: give up
    left -= got;
    // A fast client never makes a send wait: let lower priorities run.
    if ((sinceYield += got) >= 16384) { sinceYield = 0; vTaskDelay(1); }
  }
  {
    LoggerSdLock lock(SD_LOCK_WEB_MS);
    f.close();
  }
  Serial.printf("[..] sent %s: %lu of %lu bytes in %lu ms%s\n", n,
                (unsigned long)(total - left), (unsigned long)total,
                (unsigned long)(millis() - t0),
                !left ? "" : dlAbort ? " - stopped (AP going down)" : " - cut short");
  dlBusy = false;
  // Closed either way, as "Connection: close" said: cut short, the client
  // sees the transfer end rather than waiting on a Content-Length that will
  // never be met.
  httpd_sess_trigger_close(r->handle, httpd_req_to_sockfd(r));
  return ESP_OK;
}

// No delete on these pages: a drive log was often the only copy of a run
// that is expensive or impossible to repeat - a hot-engine oil-temp trace, a
// track session - and a mistap here should not destroy it. Deleting is the
// app's, for drive logs only and behind its confirmation (handleDelete,
// /api/delete): by then the phone holds a copy of every log it synced, and
// without it a drive deleted on the phone came back with the next sync.

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
// its format, as it did when the page was one snprintf. In PSRAM: internal
// RAM is what WiFi runs on (B16).
static esp_err_t sendPage(httpd_req_t *r, Page page, const char *path, const char *body, ...)
    __attribute__((format(printf, 4, 5)));
static esp_err_t sendPage(httpd_req_t *r, Page page, const char *path, const char *body, ...) {
  const size_t cap = 6144;
  char *b = (char *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
  if (!b) return replyText(r, 500, "out of memory");
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
  httpd_resp_set_hdr(r, "Cache-Control", "no-store, must-revalidate");
  Serial.printf("[HTTP] GET %s -> %u bytes (cap %u)\n", path, (unsigned)n, (unsigned)cap);
  if (n >= cap) n = cap - 1;
  const esp_err_t e = reply(r, 200, "text/html", b, n);
  heap_caps_free(b);
  return e;
}

// Gauge: what the screen shows and when it warns.
static esp_err_t handleGaugePage(httpd_req_t *r) {
  Serial.println(F("[AP] page requested"));
  return sendPage(r, PAGE_GAUGE, "/",
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
static esp_err_t handleCarPage(httpd_req_t *r) {
  return sendPage(r, PAGE_CAR, "/car",
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

static esp_err_t handleDevicePage(httpd_req_t *r) {
  return sendPage(r, PAGE_DEVICE, "/device",
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

// Anything not registered - and that means every OS connectivity probe:
// /generate_204 (Android), /hotspot-detect.html (Apple), /connecttest.txt and
// /ncsi.txt (Windows) - gets sent to the settings page. Seeing that instead
// of the expected 204 or "Success" is precisely what makes a device decide it
// is behind a captive portal and offer to open it.
static esp_err_t handlePortal(httpd_req_t *r, httpd_err_code_t) {
  // Logged because it is the fork in diagnosing a portal that will not open:
  // either the probe requests arrive here (the phone is asking, and it is our
  // answer it dislikes) or nothing arrives at all (the phone never routed the
  // probe to us). Guessing between those two wastes flash cycles.
  char host[64] = "";
  httpd_req_get_hdr_value_str(r, "Host", host, sizeof(host));
  Serial.printf("[AP] probe %s %s host=%s\n", r->method == HTTP_GET ? "GET" : "other", r->uri, host);
  // A real body, not an empty 302. Android's sendHttpProbe() classifies any
  // 2xx/3xx other than 204 as a portal, so the status was never the problem -
  // but it explicitly treats a 200 whose Content-Length is <= 4 as FAILED
  // rather than as a portal, and iOS wants content rather than a bare
  // redirect. A 200 carrying a meta-refresh satisfies both, and is what the
  // ESP32 portal projects that are confirmed working on Android actually
  // return.
  char url[40], body[320];
  snprintf(url, sizeof(url), "http://%s/", ipText.c_str());
  const int n = snprintf(body, sizeof(body),
      "<!DOCTYPE html><html><head><meta http-equiv='refresh' content='0;url=%s'>"
      "</head><body>i30 N gauge settings: <a href='%s'>%s</a></body></html>", url, url, url);
  httpd_resp_set_hdr(r, "Location", url);
  return reply(r, 200, "text/html", body, (size_t)n);
}

// ---- up and down ---------------------------------------------------------------
static bool startServer() {
  httpd_config_t c = HTTPD_DEFAULT_CONFIG();
  c.task_priority     = TASK_WEB_PRIO;
  c.stack_size        = TASK_WEB_STACK;
  c.core_id           = TASK_CORE;
  c.max_req_hdr_len   = WEB_MAX_HDR_LEN;
  c.max_uri_handlers  = 16;
  c.max_open_sockets  = 5;
  // A sixth connection closes the least recently used one, rather than
  // being refused while a phone's dead keep-alive connections fill the table.
  c.lru_purge_enable  = true;
  c.recv_wait_timeout = WEB_RECV_TIMEOUT_S;
  c.send_wait_timeout = WEB_SEND_TIMEOUT_S;
  if (httpd_start(&httpd, &c) != ESP_OK) { httpd = nullptr; return false; }
  // Explicit methods: a read-only route takes no body, so /save, /update,
  // /api/clock and /api/delete are the only ones that read one.
  static const httpd_uri_t routes[] = {
    {"/",             HTTP_GET,  handleGaugePage,    nullptr},
    {"/car",          HTTP_GET,  handleCarPage,      nullptr},
    {"/device",       HTTP_GET,  handleDevicePage,   nullptr},
    {"/save",         HTTP_POST, handleSave,         nullptr},
    {"/update",       HTTP_POST, handleUpdate,       nullptr},
    {"/api/clock",    HTTP_POST, handleClockSet,     nullptr},
    {"/api/settings", HTTP_GET,  handleSettingsJson, nullptr},
    {"/api/live",     HTTP_GET,  handleLiveJson,     nullptr},
    {"/files",        HTTP_GET,  handleFiles,        nullptr},
    {"/api/files",    HTTP_GET,  handleFilesJson,    nullptr},
    {"/dl",           HTTP_GET,  handleDownload,     nullptr},
    {"/api/delete",   HTTP_POST, handleDelete,       nullptr},
  };
  for (const httpd_uri_t &u : routes) httpd_register_uri_handler(httpd, &u);
  httpd_register_err_handler(httpd, HTTPD_404_NOT_FOUND, handlePortal);
  return true;
}

void web_stop() {
  if (!up) return;
  // The server's handlers end first: a download (sleep unmounts the card
  // next), an upload, one waiting for loop() - which is here, not serving it.
  // httpd_stop() waits for the running one; each sees these and stops.
  stopping = true;
  dlAbort = true;
  if (httpd) { httpd_stop(httpd); httpd = nullptr; }
  stopping = false;
  dlAbort = false;
  dlBusy = false;
  apRestartAt = 0;
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  up = false;
  ipText = "";
  Serial.println(F("[..] settings AP stopped"));
}

// Times web_start() declined for want of internal RAM, since boot - in the
// drive log, where the serial console's "[ERR] not starting AP" never is.
static uint32_t apRefused = 0;
uint32_t web_ap_refused() { return apRefused; }

void web_start() {
  if (up) return;
  loopTask = xTaskGetCurrentTaskHandle();   // called from loop()

  // esp_wifi takes its buffers from internal RAM and is NOT defensive about
  // running out: a failed allocation deep inside ieee80211_hostap_attach
  // surfaces as a null-pointer panic, not a false return. That really happened
  // here - LVGL's 128 KB pool used to sit in internal DRAM, leaving 52 KB, and
  // every long press rebooted the board. WiFi.mode() alone costs ~46 KB.
  // So check first and refuse with a message you can actually act on.
  const size_t freeInternal =
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (freeInternal < AP_MIN_INTERNAL_HEAP) {
    apRefused++;
    Serial.printf("[ERR] not starting AP: %u B internal RAM free, need %u\n",
                  (unsigned)freeInternal, (unsigned)AP_MIN_INTERNAL_HEAP);
    return;
  }

  // Report associations and DHCP leases, so "the phone never joined" can be
  // told apart from "it joined and then asked nothing". Once per boot: the AP
  // restarts after every drive (AP_DRIVE_OFF), and each onEvent() adds a
  // handler - a day of drives piled them up, each printing every event.
  static bool eventsHooked = false;
  if (!eventsHooked) {
    eventsHooked = true;
    WiFi.onEvent([](arduino_event_id_t e, arduino_event_info_t info) {
      (void)info;
      if (e == ARDUINO_EVENT_WIFI_AP_STACONNECTED)         Serial.println(F("[AP] station connected"));
      else if (e == ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED)   Serial.println(F("[AP] lease handed out"));
      else if (e == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) Serial.println(F("[AP] station left"));
    });
  }

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
  if (!startServer()) {
    Serial.println(F("[ERR] web server did not start"));
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    return;
  }

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
  web_beacon_update();
  Serial.printf("[OK] settings AP '%s' up at http://%s/  portal:dhcp=%s dns=%s\n",
                settings.apSsid, ipText.c_str(),
                portalOk ? "yes" : "NO", dnsOk ? "yes" : "NO");
}

// ---- the beacon -----------------------------------------------------------------
// The AP's own beacons - sent ten times a second anyway, what puts the network
// in a phone's list - carry one vendor element: the last drive's fingerprint
// (logcrypt_beacon). The app reads it from the phone's WiFi scan results,
// without joining, and tells whether the gauge has a drive it has not
// (asked 2026-10-22). In probe responses too, for an active scan. Nothing
// new on the air and no RAM worth counting: Bluetooth was the first idea,
// and its stack cannot start beside the AP for want of internal RAM
// (measured: a crash).
//
//   0xDD len | OUI 02 4E 47 ("NG", locally administered) | type 1 | 8 bytes
static constexpr uint8_t BEACON_OUI[3] = {0x02, 0x4E, 0x47};

void web_beacon_update() {
  if (!up) return;
  char session[7];
  if (!settings_last_drive(session)) return;      // no drive yet: nothing to say
  static uint8_t ie[2 + 4 + 8];
  ie[0] = 0xDD;                                   // vendor specific
  ie[1] = sizeof(ie) - 2;
  memcpy(ie + 2, BEACON_OUI, 3);
  ie[5] = 1;                                      // this format, version 1
  // The key's public half, or zeros with none - settings.logPub is zeroed then.
  logcrypt_beacon(ie + 6, settings.logPub, session);
  // Replaced, not added to: off first, then on with the new bytes.
  for (wifi_vendor_ie_type_t t : {WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_TYPE_PROBE_RESP}) {
    esp_wifi_set_vendor_ie(false, t, WIFI_VND_IE_ID_0, nullptr);
    if (esp_wifi_set_vendor_ie(true, t, WIFI_VND_IE_ID_0, ie) != ESP_OK)
      Serial.println(F("[ERR] beacon element not set"));
  }
}

bool web_is_up()      { return up; }
uint8_t web_clients() { return up ? (uint8_t)WiFi.softAPgetStationNum() : 0; }
const char *web_ip()  { return ipText.c_str(); }

// On loop(), every pass: what the server's handlers left for it.
void web_task() {
  serveLoopJob();
  showPostedNotice();
  if (rebootAt && calc_reached(millis(), rebootAt)) {
    rebootAt = 0;
    ota_reboot();
  }
  if (!up) return;
  // A non-blocking poll. The portal sheet only appears if the name lookup is
  // answered before the HTTP probe is attempted.
  dns.processNextRequest();
  if (apRestartAt && calc_reached(millis(), apRestartAt)) {
    apRestartAt = 0;
    restartAp();
  }
}
