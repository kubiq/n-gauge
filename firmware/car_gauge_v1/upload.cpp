// ============================================================================
// upload.cpp  —  see upload.h.
// ============================================================================
#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <SD_MMC.h>
#include <WiFi.h>
#include <WireGuard-ESP32.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <mbedtls/base64.h>
#include <time.h>
#include "upload.h"
#include "logger.h"     // LoggerSdLock, logger_card_ok, logger_note_boot
#include "monocypher.h" // crypto_x25519_public_key: the gauge's WireGuard key
#include "podstate.h"   // the car is on: the time to go
#include "rtc.h"        // clock_valid: the handshake carries a timestamp
#include "webapi.h"     // web_is_up, web_clients

// ---- timings ---------------------------------------------------------------
#define UP_JOIN_MS       90000   // the hotspot comes up after the phone's BT does
#define UP_HANDSHAKE_MS  30000   // the tunnel's first request waits for it
#define UP_MAX_MS       600000   // the whole upload, at most
#define UP_HTTP_MS       10000
#define UP_CHUNK          4096   // a card read, under the lock (internal: SD DMA)
#define UP_PLAN_MAX    (48 * 1024)
#define UP_TASK_STACK     8192

// ---- the settings, NVS namespace "upload" ----------------------------------
struct UpConfig {
  char     ssid[33], pass[64];        // the hotspot
  char     url[96];                   // the server, as seen inside the tunnel
  char     token[65];
  char     wgAddr[16];                // the gauge's address in the tunnel
  char     wgPeer[45];                // the router's public key, base64
  char     wgHost[64];                // the router, from outside
  uint16_t wgPort;
  uint8_t  wgPriv[32];                // made here, never leaves
};
static UpConfig cfg;
static char wgPub[45];                // its public half, base64

static const char *NS = "upload";
const char *const UPLOAD_KEYS[] = {"upSsid", "upPass", "upUrl", "upToken", "wgAddr",
                                   "wgPeer", "wgHost", "wgPort", "wgNewKey", nullptr};

static void b64(char *out, size_t n, const uint8_t *in, size_t len) {
  size_t olen = 0;
  if (mbedtls_base64_encode((unsigned char *)out, n, &olen, in, len) != 0) olen = 0;
  out[olen] = '\0';
}

static void newKey() {
  esp_fill_random(cfg.wgPriv, sizeof(cfg.wgPriv));
  cfg.wgPriv[0] &= 248;                // Curve25519's clamping, as wg genkey
  cfg.wgPriv[31] = (cfg.wgPriv[31] & 127) | 64;
}

static void derivePub() {
  uint8_t pub[32];
  crypto_x25519_public_key(pub, cfg.wgPriv);
  b64(wgPub, sizeof(wgPub), pub, sizeof(pub));
}

static void save() {
  Preferences p;
  if (!p.begin(NS, false)) return;
  p.putString("ssid", cfg.ssid);
  p.putString("pass", cfg.pass);
  p.putString("url", cfg.url);
  p.putString("token", cfg.token);
  p.putString("wgAddr", cfg.wgAddr);
  p.putString("wgPeer", cfg.wgPeer);
  p.putString("wgHost", cfg.wgHost);
  p.putUShort("wgPort", cfg.wgPort);
  p.putBytes("wgPriv", cfg.wgPriv, sizeof(cfg.wgPriv));
  p.end();
}

static void getStr(Preferences &p, const char *k, char *out, size_t n) {
  String s = p.getString(k, "");
  strlcpy(out, s.c_str(), n);
}

void upload_init() {
  memset(&cfg, 0, sizeof(cfg));
  cfg.wgPort = 51820;
  bool haveKey = false;
  Preferences p;
  if (p.begin(NS, true)) {
    getStr(p, "ssid", cfg.ssid, sizeof(cfg.ssid));
    getStr(p, "pass", cfg.pass, sizeof(cfg.pass));
    getStr(p, "url", cfg.url, sizeof(cfg.url));
    getStr(p, "token", cfg.token, sizeof(cfg.token));
    getStr(p, "wgAddr", cfg.wgAddr, sizeof(cfg.wgAddr));
    getStr(p, "wgPeer", cfg.wgPeer, sizeof(cfg.wgPeer));
    getStr(p, "wgHost", cfg.wgHost, sizeof(cfg.wgHost));
    cfg.wgPort = p.getUShort("wgPort", 51820);
    haveKey = p.getBytes("wgPriv", cfg.wgPriv, sizeof(cfg.wgPriv)) == sizeof(cfg.wgPriv);
    p.end();
  }
  if (!haveKey) {                      // first boot with this: the gauge's own key
    newKey();
    save();
    Serial.println(F("[OK] upload: made the gauge's WireGuard key"));
  }
  derivePub();
}

static bool configured() {
  return cfg.ssid[0] && cfg.url[0] && cfg.wgAddr[0] && cfg.wgPeer[0] && cfg.wgHost[0];
}

// A value for a char[] field: within its size, printable, no spaces at the
// ends (a pasted key often has a newline).
static bool setText(char *field, size_t n, const char *v, size_t minLen) {
  while (*v == ' ' || *v == '\t') v++;
  size_t len = strlen(v);
  while (len && (v[len - 1] == ' ' || v[len - 1] == '\n' || v[len - 1] == '\r' || v[len - 1] == '\t')) len--;
  if (len >= n || (len && len < minLen)) return false;
  for (size_t i = 0; i < len; i++) if ((uint8_t)v[i] < 32 || (uint8_t)v[i] > 126) return false;
  memcpy(field, v, len);
  field[len] = '\0';
  return true;
}

bool upload_setting(const char *key, const char *v) {
  bool ok;
  if      (!strcmp(key, "upSsid"))  ok = setText(cfg.ssid, sizeof(cfg.ssid), v, 1);
  else if (!strcmp(key, "upPass"))  ok = setText(cfg.pass, sizeof(cfg.pass), v, 8);   // WPA2's least
  else if (!strcmp(key, "upUrl"))   ok = (!*v || !strncmp(v, "http://", 7)) && setText(cfg.url, sizeof(cfg.url), v, 8);
  else if (!strcmp(key, "upToken")) ok = setText(cfg.token, sizeof(cfg.token), v, 0);
  else if (!strcmp(key, "wgAddr"))  ok = (!*v || IPAddress().fromString(v)) && setText(cfg.wgAddr, sizeof(cfg.wgAddr), v, 7);
  else if (!strcmp(key, "wgPeer"))  ok = setText(cfg.wgPeer, sizeof(cfg.wgPeer), v, 44);
  else if (!strcmp(key, "wgHost"))  ok = setText(cfg.wgHost, sizeof(cfg.wgHost), v, 1);
  else if (!strcmp(key, "wgPort")) {
    const long port = atol(v);
    ok = port > 0 && port < 65536;
    if (ok) cfg.wgPort = (uint16_t)port;
  } else if (!strcmp(key, "wgNewKey")) {
    newKey();
    derivePub();
    ok = true;
  } else return false;
  if (ok) save();
  return ok;
}

// ---- the result, for /api/upload and the boot log --------------------------
struct UpResult {
  uint32_t at;                        // epoch s, 0 = none this boot
  bool     ok;
  uint16_t files;
  uint32_t bytes, ms;
  char     msg[72];
};
static UpResult last = {};

static const char *jsonSafe(const char *s, char *out, size_t n) {
  size_t o = 0;
  for (; *s && o + 2 < n; s++) {
    if (*s == '"' || *s == '\\') out[o++] = '\\';
    out[o++] = *s;
  }
  out[o] = '\0';
  return out;
}

int upload_json(char *out, size_t n) {
  char ssid[70], url[200], host[130], msg[150], addr[34], peer[92];
  const int len = snprintf(out, n,
      "{\"configured\":%s,\"ssid\":\"%s\",\"passSet\":%s,\"url\":\"%s\",\"tokenSet\":%s,"
      "\"wgAddr\":\"%s\",\"wgPeer\":\"%s\",\"wgHost\":\"%s\",\"wgPort\":%u,\"wgPub\":\"%s\","
      "\"last\":{\"at\":%lu,\"ok\":%s,\"files\":%u,\"bytes\":%lu,\"ms\":%lu,\"msg\":\"%s\"}}",
      configured() ? "true" : "false", jsonSafe(cfg.ssid, ssid, sizeof(ssid)),
      cfg.pass[0] ? "true" : "false", jsonSafe(cfg.url, url, sizeof(url)),
      cfg.token[0] ? "true" : "false", jsonSafe(cfg.wgAddr, addr, sizeof(addr)),
      jsonSafe(cfg.wgPeer, peer, sizeof(peer)),
      jsonSafe(cfg.wgHost, host, sizeof(host)), cfg.wgPort, wgPub,
      (unsigned long)last.at, last.ok ? "true" : "false", last.files,
      (unsigned long)last.bytes, (unsigned long)last.ms, jsonSafe(last.msg, msg, sizeof(msg)));
  return (len > 0 && (size_t)len < n) ? len : 0;
}

// ---- the card, read a chunk at a time under the logger's lock ---------------
// For HTTPClient's PUT: the logger keeps writing the drive's files meanwhile.
class CardStream : public Stream {
 public:
  CardStream(File &f, uint32_t len, uint8_t *buf) : f_(f), left_(len), buf_(buf) {}
  // What is still to come. -1 once a read failed: HTTPClient's send loop
  // runs while this is > -1, and 0 would keep it waiting.
  int available() override { return failed_ ? -1 : (int)(have_ - pos_ + left_); }
  int read() override { return fill() ? buf_[pos_++] : -1; }
  int peek() override { return fill() ? buf_[pos_] : -1; }
  size_t readBytes(char *out, size_t n) override {
    size_t got = 0;
    while (got < n && fill()) {
      const size_t take = min(n - got, have_ - pos_);
      memcpy(out + got, buf_ + pos_, take);
      pos_ += take;
      got += take;
    }
    return got;
  }
  size_t write(uint8_t) override { return 0; }
  bool failed() const { return failed_; }
 private:
  bool fill() {
    if (pos_ < have_) return true;
    if (!left_ || failed_) return false;
    LoggerSdLock lock(SD_LOCK_WEB_MS);
    if (!lock || logger_card_shared()) { failed_ = true; return false; }
    const size_t got = f_.read(buf_, left_ < UP_CHUNK ? left_ : UP_CHUNK);
    if (!got) { failed_ = true; return false; }
    have_ = got;
    pos_ = 0;
    left_ -= got;
    return true;
  }
  File &f_;
  uint32_t left_;
  uint8_t *buf_;
  size_t have_ = 0, pos_ = 0;
  bool failed_ = false;
};

// The logs: what the gauge writes, by their endings. Not firmware.bin, not
// a folder, nothing a name the server would refuse.
static bool isLog(const char *n) {
  const size_t len = strlen(n);
  if (!len || len > 95 || n[0] == '.') return false;
  for (size_t i = 0; i < len; i++)
    if (!isalnum((unsigned char)n[i]) && n[i] != '_' && n[i] != '.' && n[i] != '-') return false;
  static const char *const ends[] = {".nge", ".csv", ".gpx", ".vbo", ".txt"};
  for (const char *e : ends) {
    const size_t el = strlen(e);
    if (len > el && !strcmp(n + len - el, e)) return true;
  }
  return false;
}

// "name size" per line, into plan; its length, 0 if the card was not there.
static size_t listCard(char *plan, size_t cap, uint16_t *count) {
  size_t n = 0;
  *count = 0;
  File root;
  {
    LoggerSdLock lock(SD_LOCK_WEB_MS);
    if (!lock || !logger_card_ok() || logger_card_shared()) return 0;
    root = SD_MMC.open("/");
  }
  if (!root) return 0;
  for (;;) {
    LoggerSdLock lock(SD_LOCK_WEB_MS);
    if (!lock) break;
    File e = root.openNextFile();
    if (!e) break;
    if (!e.isDirectory() && isLog(e.name()) && n + 120 < cap) {
      n += snprintf(plan + n, cap - n, "%s %lu\n", e.name(), (unsigned long)e.size());
      (*count)++;
    }
    e.close();
  }
  LoggerSdLock lock(SD_LOCK_WEB_MS);
  root.close();
  return n;
}

// ---- the upload, on its own task ------------------------------------------
// IDLE -> WAIT_AP (the radio asked for; apTask takes the AP down) -> RUNNING
// (the task has it) -> IDLE once the task says done. Loop-side only, but
// the task's `taskDone`.
enum UpState : uint8_t { UP_IDLE, UP_WAIT_AP, UP_RUNNING };
static UpState state = UP_IDLE;
static volatile bool taskDone = false;
static bool requested = false, triedThisBoot = false;

static void finish(UpResult &r, uint32_t t0, bool ok, const char *msg) {
  r.ok = ok;
  r.ms = millis() - t0;
  r.at = clock_valid() ? (uint32_t)time(nullptr) : 0;
  strlcpy(r.msg, msg, sizeof(r.msg));
}

static void authorise(HTTPClient &http, const UpConfig &c) {
  if (!c.token[0]) return;
  char h[80];
  snprintf(h, sizeof(h), "Bearer %s", c.token);
  http.addHeader("Authorization", h);
}

// One file's missing bytes [from, size now). 0 on success, else a reason.
static const char *putFile(const UpConfig &c, const char *name, uint32_t from, uint8_t *buf,
                           uint32_t *sent) {
  char path[100];
  snprintf(path, sizeof(path), "/%s", name);
  File f;
  uint32_t size = 0;
  {
    LoggerSdLock lock(SD_LOCK_WEB_MS);
    if (!lock || logger_card_shared()) return "card busy";
    f = SD_MMC.open(path, FILE_READ);
    if (f) size = f.size();
    if (f && from && !f.seek(from)) { f.close(); return "seek failed"; }
  }
  if (!f) return "file gone";
  const char *err = nullptr;
  if (size > from) {
    char url[200];
    snprintf(url, sizeof(url), "%s/f/%s?offset=%lu", c.url, name, (unsigned long)from);
    HTTPClient http;
    http.setConnectTimeout(UP_HTTP_MS);
    http.setTimeout(UP_HTTP_MS);
    http.begin(url);
    authorise(http, c);
    http.addHeader("Content-Type", "application/octet-stream");
    CardStream s(f, size - from, buf);
    const int code = http.sendRequest("PUT", &s, size - from);
    if (code == 200 && !s.failed()) *sent += size - from;
    else err = code == 409 ? "server holds another size" : s.failed() ? "card read failed" : "PUT failed";
    http.end();
  }
  LoggerSdLock lock(SD_LOCK_WEB_MS);
  f.close();
  return err;
}

static void uploadTask(void *arg) {
  UpConfig &c = *(UpConfig *)arg;
  UpResult r = {};
  const uint32_t t0 = millis();
  char line[160];
  uint8_t *buf = (uint8_t *)heap_caps_malloc(UP_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  char *plan = (char *)heap_caps_malloc(UP_PLAN_MAX, MALLOC_CAP_SPIRAM);
  WireGuard wg;
  bool wgUp = false;
  uint16_t onCard = 0;
  String todo;

  if (!buf || !plan) { finish(r, t0, false, "no memory"); goto out; }

  WiFi.mode(WIFI_STA);
  WiFi.begin(c.ssid, c.pass);
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < UP_JOIN_MS) delay(250);
  if (WiFi.status() != WL_CONNECTED) { finish(r, t0, false, "no hotspot"); goto out; }
  Serial.printf("[..] upload: joined the hotspot in %lu ms, RSSI %d\n",
                (unsigned long)(millis() - t0), WiFi.RSSI());

  {
    IPAddress addr;
    addr.fromString(c.wgAddr);
    char priv[45];
    b64(priv, sizeof(priv), c.wgPriv, sizeof(c.wgPriv));
    wgUp = wg.begin(addr, priv, c.wgHost, c.wgPeer, c.wgPort);
    memset(priv, 0, sizeof(priv));
  }
  if (!wgUp) { finish(r, t0, false, "tunnel did not start"); goto out; }

  {
    const size_t len = listCard(plan, UP_PLAN_MAX, &onCard);
    if (!len) { finish(r, t0, false, "card not readable"); goto out; }
    // The first request waits for the handshake: tried until it answers.
    const uint32_t tw = millis();
    int code = -1;
    char url[120];
    snprintf(url, sizeof(url), "%s/plan", c.url);
    while (millis() - tw < UP_HANDSHAKE_MS) {
      HTTPClient http;
      http.setConnectTimeout(5000);
      http.setTimeout(UP_HTTP_MS);
      http.begin(url);
      authorise(http, c);
      http.addHeader("Content-Type", "text/plain");
      code = http.POST((uint8_t *)plan, len);
      if (code == 200) todo = http.getString();
      http.end();
      if (code > 0) break;
      delay(2000);
    }
    if (code != 200) {
      snprintf(line, sizeof(line), code == 401 ? "server refused the token"
                                   : code > 0 ? "plan answered %d" : "server not reached", code);
      finish(r, t0, false, line);
      goto out;
    }
  }

  // "name offset" per line: each file's missing part.
  {
    const char *err = nullptr;
    int from = 0;
    while (from < (int)todo.length() && millis() - t0 < UP_MAX_MS) {
      int nl = todo.indexOf('\n', from);
      if (nl < 0) nl = todo.length();
      const String l = todo.substring(from, nl);
      from = nl + 1;
      const int sp = l.indexOf(' ');
      if (sp <= 0) continue;
      const String name = l.substring(0, sp);
      if (!isLog(name.c_str())) continue;
      err = putFile(c, name.c_str(), (uint32_t)l.substring(sp + 1).toInt(), buf, &r.bytes);
      if (err) {
        snprintf(line, sizeof(line), "%s: %s", name.c_str(), err);
        break;
      }
      r.files++;
    }
    if (err) finish(r, t0, false, line);
    else if (millis() - t0 >= UP_MAX_MS) finish(r, t0, false, "out of time");
    else finish(r, t0, true, "");
  }

out:
  if (wgUp) wg.end();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  free(buf);
  free(plan);
  snprintf(line, sizeof(line), "upload: %s - %u files, %lu KB in %lu ms (%u logs on the card)",
           r.ok ? "done" : r.msg, r.files, (unsigned long)(r.bytes / 1024), (unsigned long)r.ms,
           onCard);
  Serial.println(line);
  logger_note_boot(line);
  last = r;
  taskDone = true;
  vTaskDelete(nullptr);
}

// ---- when ------------------------------------------------------------------
void upload_now() {
  requested = true;
  triedThisBoot = false;
}

bool upload_holds_radio() { return state != UP_IDLE; }
bool upload_busy() { return state != UP_IDLE; }

void upload_task(uint32_t now) {
  (void)now;
  static UpConfig taskCfg;            // the task's copy: /save may change cfg
  if (state == UP_RUNNING) {
    if (taskDone) state = UP_IDLE;    // the radio goes back to the AP
    return;
  }
  if (state == UP_WAIT_AP) {
    if (web_is_up()) return;          // apTask takes it down: it sees the hold
    taskCfg = cfg;
    taskDone = false;
    state = UP_RUNNING;
    if (xTaskCreatePinnedToCore(uploadTask, "upload", UP_TASK_STACK, &taskCfg, 1, nullptr, 0) != pdPASS) {
      state = UP_IDLE;
      Serial.println(F("[ERR] upload: no task"));
    }
    return;
  }
  if (triedThisBoot || !configured()) return;
  // The car is on (talking), the clock is right (the handshake carries a
  // timestamp the router checks), the card is in and the gauge's own, and
  // nobody is on the AP. Or the bench says now.
  const bool carOn = podstate().car != PS_CAR_OFF;
  if (!(carOn || requested) || !clock_valid() || !logger_card_ok() || logger_card_shared() ||
      web_clients() > 0)
    return;
  requested = false;
  triedThisBoot = true;
  state = UP_WAIT_AP;
  Serial.println(F("[..] upload: the car is on - the radio is the upload's"));
}
