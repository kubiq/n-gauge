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
#include <esp_sntp.h>    // the clock from NTP, when the gauge has none
#include <lwip/dns.h>    // dns_setserver: the server's name by public resolvers
#include <lwip/tcpip.h>  // LOCK_TCPIP_CORE around it
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
#define UP_DIRECT_MS      3000   // the server tried without the tunnel, at home
#define UP_NTP_MS        15000   // the clock, when the gauge has none
// The whole upload, at most: a setting (upMaxMin, the app's Upload card),
// so the phone's hotspot is not held longer than its owner wants. What is
// left goes the next time, from where this one stopped.
#define UP_MAX_MIN_DEFAULT  10
#define UP_MAX_MIN_TOP      60
#define UP_HTTP_MS       10000
// A failed try (no hotspot yet, no answer) is tried again this much later,
// up to UP_TRIES per wake.
#define UP_RETRY_MS     300000
#define UP_TRIES             3
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
  char     wgDns[16];                 // a DNS inside the tunnel, for the server's name; "" = the hotspot's
  uint16_t wgPort;
  uint8_t  maxMin;                    // the time limit, minutes
  uint8_t  wgPriv[32];                // made here, never leaves
};
static UpConfig cfg;
static char wgPub[45];                // its public half, base64

static const char *NS = "upload";
const char *const UPLOAD_KEYS[] = {"upSsid", "upPass", "upUrl", "upToken", "wgAddr",
                                   "wgPeer", "wgHost", "wgPort", "wgDns", "upMaxMin", "wgNewKey", nullptr};

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
  p.putString("wgDns", cfg.wgDns);
  p.putUShort("wgPort", cfg.wgPort);
  p.putUChar("maxMin", cfg.maxMin);
  p.putBytes("wgPriv", cfg.wgPriv, sizeof(cfg.wgPriv));
  p.end();
}

static void getStr(Preferences &p, const char *k, char *out, size_t n) {
  String s = p.getString(k, "");
  strlcpy(out, s.c_str(), n);
}

// WireGuard.cpp's step in end(), kept across a reset: 0x5747000N.
extern volatile uint32_t wg_end_step;
static uint8_t endStepSeen = 0;         // printed from upload_task, after USB is up

// ---- the result, for /api/upload and the boot log --------------------------
struct UpResult {
  uint32_t at;                        // epoch s, 0 = none yet / no clock
  bool     ok;
  uint16_t files;
  uint32_t bytes, ms;
  char     msg[72];
};
// The last try's result, kept in NVS ("last") since 2026-11-07: in RAM only,
// it was gone by the time the app could ask - the gauge sleeps after the
// drive the upload was tried on.
static UpResult last = {};

static void saveLast() {
  Preferences p;
  if (!p.begin(NS, false)) return;
  p.putBytes("last", &last, sizeof(last));
  p.end();
}

void upload_init() {
  if ((wg_end_step & 0xFFFFFFF0u) == 0x57470000u) endStepSeen = wg_end_step & 0xF;
  wg_end_step = 0;
  memset(&cfg, 0, sizeof(cfg));
  cfg.wgPort = 51820;
  cfg.maxMin = UP_MAX_MIN_DEFAULT;
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
    getStr(p, "wgDns", cfg.wgDns, sizeof(cfg.wgDns));
    cfg.wgPort = p.getUShort("wgPort", 51820);
    cfg.maxMin = p.getUChar("maxMin", UP_MAX_MIN_DEFAULT);
    haveKey = p.getBytes("wgPriv", cfg.wgPriv, sizeof(cfg.wgPriv)) == sizeof(cfg.wgPriv);
    if (p.getBytes("last", &last, sizeof(last)) != sizeof(last)) last = {};
    last.msg[sizeof(last.msg) - 1] = '\0';
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

// Saved whatever it is (2026-11-06: refusing a value made it typed again
// and again) - cleaned up where the usual forms differ, and a word on what
// still looks wrong. Spaces and newlines at the ends go (a pasted key often
// has one); too long is cut; a character outside printable ASCII is dropped.
static void put(char *field, size_t n, const char *v) {
  while (*v == ' ' || *v == '\t') v++;
  size_t len = strlen(v);
  while (len && (v[len - 1] == ' ' || v[len - 1] == '\n' || v[len - 1] == '\r' || v[len - 1] == '\t')) len--;
  size_t o = 0;
  for (size_t i = 0; i < len && o + 1 < n; i++)
    if ((uint8_t)v[i] >= 32 && (uint8_t)v[i] <= 126) field[o++] = v[i];
  field[o] = '\0';
}

const char *upload_setting(const char *key, const char *v) {
  const char *note = nullptr;
  if (!strcmp(key, "upSsid")) {
    put(cfg.ssid, sizeof(cfg.ssid), v);
  } else if (!strcmp(key, "upPass")) {
    put(cfg.pass, sizeof(cfg.pass), v);
    if (strlen(cfg.pass) < 8) note = "the hotspot password is under 8 characters";
  } else if (!strcmp(key, "upUrl")) {
    // "192.168.1.10:8650" is taken as http://; a trailing / goes.
    char u[sizeof(cfg.url)];
    put(u, sizeof(u), v);
    if (u[0] && !strstr(u, "://")) snprintf(cfg.url, sizeof(cfg.url), "http://%s", u);
    else strlcpy(cfg.url, u, sizeof(cfg.url));
    size_t l = strlen(cfg.url);
    while (l && cfg.url[l - 1] == '/') cfg.url[--l] = '\0';
    if (cfg.url[0] && strncmp(cfg.url, "http://", 7)) note = "the server must be http:// (the tunnel encrypts)";
  } else if (!strcmp(key, "upToken")) {
    put(cfg.token, sizeof(cfg.token), v);
  } else if (!strcmp(key, "wgAddr")) {
    put(cfg.wgAddr, sizeof(cfg.wgAddr), v);
    char *slash = strchr(cfg.wgAddr, '/');      // "10.0.0.7/32", as the router shows it
    if (slash) *slash = '\0';
    IPAddress ip;
    if (cfg.wgAddr[0] && !ip.fromString(cfg.wgAddr)) note = "the tunnel address is not an IP address";
  } else if (!strcmp(key, "wgPeer")) {
    put(cfg.wgPeer, sizeof(cfg.wgPeer), v);
    if (cfg.wgPeer[0] && strlen(cfg.wgPeer) != 44) note = "the router's public key is not 44 characters";
  } else if (!strcmp(key, "wgDns")) {
    put(cfg.wgDns, sizeof(cfg.wgDns), v);
    IPAddress ip;
    if (cfg.wgDns[0] && !ip.fromString(cfg.wgDns)) note = "the DNS in the tunnel is not an IP address";
  } else if (!strcmp(key, "wgHost")) {
    put(cfg.wgHost, sizeof(cfg.wgHost), v);
    char *colon = strrchr(cfg.wgHost, ':');     // "vpn.example.org:51820": the port goes to wgPort
    if (colon && atol(colon + 1) > 0) {
      cfg.wgPort = (uint16_t)atol(colon + 1);
      *colon = '\0';
    }
  } else if (!strcmp(key, "wgPort")) {
    const long port = atol(v);
    if (port > 0 && port < 65536) cfg.wgPort = (uint16_t)port;
    else note = "the router port is not 1-65535";
  } else if (!strcmp(key, "upMaxMin")) {
    const long m = atol(v);
    cfg.maxMin = (uint8_t)(m < 1 ? 1 : m > UP_MAX_MIN_TOP ? UP_MAX_MIN_TOP : m);
    if (m < 1 || m > UP_MAX_MIN_TOP) note = "the time limit is 1-60 minutes";
  } else if (!strcmp(key, "wgNewKey")) {
    newKey();
    derivePub();
  } else {
    return nullptr;
  }
  save();
  return note;
}

// ---- the result, for /api/upload (UpResult, above) -------------------------
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
  char ssid[70], url[200], host[130], msg[150], addr[34], peer[92], dns[34];
  const int len = snprintf(out, n,
      "{\"configured\":%s,\"ssid\":\"%s\",\"passSet\":%s,\"url\":\"%s\",\"tokenSet\":%s,"
      "\"wgAddr\":\"%s\",\"wgPeer\":\"%s\",\"wgHost\":\"%s\",\"wgPort\":%u,\"wgDns\":\"%s\","
      "\"maxMin\":%u,\"wgPub\":\"%s\","
      "\"last\":{\"at\":%lu,\"ok\":%s,\"files\":%u,\"bytes\":%lu,\"ms\":%lu,\"msg\":\"%s\"}}",
      configured() ? "true" : "false", jsonSafe(cfg.ssid, ssid, sizeof(ssid)),
      cfg.pass[0] ? "true" : "false", jsonSafe(cfg.url, url, sizeof(url)),
      cfg.token[0] ? "true" : "false", jsonSafe(cfg.wgAddr, addr, sizeof(addr)),
      jsonSafe(cfg.wgPeer, peer, sizeof(peer)),
      jsonSafe(cfg.wgHost, host, sizeof(host)), cfg.wgPort, jsonSafe(cfg.wgDns, dns, sizeof(dns)),
      cfg.maxMin, wgPub,
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
  // A tick between entries, with the lock let go: the logger waits for it
  // at the same priority on the same core, and a lock given back and taken
  // again at once never let it in - ~230 files of openNextFile() starved it
  // past the watchdog (2026-11-07: "wdt: logger", the upload's resets).
  for (;;) {
    bool more = false;
    {
      LoggerSdLock lock(SD_LOCK_WEB_MS);
      if (!lock) break;
      File e = root.openNextFile();
      if (e) {
        more = true;
        if (!e.isDirectory() && isLog(e.name()) && n + 120 < cap) {
          n += snprintf(plan + n, cap - n, "%s %lu\n", e.name(), (unsigned long)e.size());
          (*count)++;
        }
        e.close();
      }
    }
    if (!more) break;
    vTaskDelay(1);
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
static bool requested = false;
// Per wake: done (uploaded, or stopped by its time limit - the rest goes
// next time), or how many tries failed and when the next may start. A
// failure used to end it for the wake: on 2026-11-07 a try at the door,
// before the phone's hotspot was up, left the drive with none.
static bool doneThisWake = false;
static uint8_t triesFailed = 0;
static uint32_t retryAtMs = 0;

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

// ---- the server's address ---------------------------------------------------
// Looked up once per upload, before the tunnel, by public resolvers - not
// the hotspot's. On mobile data (2026-11-07) the carrier's DNS answered the
// server's name with a public address, 1.1.1.1 and 8.8.8.8 with the LAN one
// the home DNS gives: through the tunnel only the LAN one leads to the
// server, so four uploads ended "tunnel up, server not reached". At home,
// on a hotspot sharing the home WiFi, the router answered - and it worked.
// Then each request connects to that address itself and hands HTTPClient
// the open connection with the name: the name still goes as Host (a proxy
// in front of the server routes by it), and no lookup happens inside the
// tunnel. HTTPClient will not take a Host header otherwise.
struct UpServer {
  char host[64];
  char base[48];                      // a path the URL has after the host, or ""
  IPAddress ip;
  uint16_t port;
};
static UpServer server;

// "http://host[:port][/base]" -> server, false if there is no host.
static bool parseServer(const char *url) {
  memset(&server, 0, sizeof(server));
  const char *p = strstr(url, "://");
  p = p ? p + 3 : url;
  size_t n = strcspn(p, ":/");
  if (!n || n >= sizeof(server.host)) return false;
  memcpy(server.host, p, n);
  p += n;
  server.port = 80;
  if (*p == ':') {
    server.port = (uint16_t)atoi(p + 1);
    p += 1 + strcspn(p + 1, "/");
  }
  strlcpy(server.base, p, sizeof(server.base));
  size_t b = strlen(server.base);
  while (b && server.base[b - 1] == '/') server.base[--b] = '\0';
  return server.port != 0;
}

// Through the tunnel, by `dns` - a DNS on the VPN's side (the Upload card's
// "DNS in the tunnel"), which knows the server's LAN address - or by the
// hotspot's DNS where none is set. Not by public resolvers before the
// tunnel (tried 2026-11-07): the router's own name then came out its public
// address, which from inside the home network (a hotspot sharing the home
// WiFi) does not answer - "no handshake from the router".
static bool resolveServer(const char *dns) {
  if (dns[0]) {
    ip_addr_t d;
    if (!ipaddr_aton(dns, &d)) return false;
    LOCK_TCPIP_CORE();
    dns_setserver(0, &d);
    dns_setserver(1, nullptr);
    UNLOCK_TCPIP_CORE();
  }
  return WiFi.hostByName(server.host, server.ip) == 1;
}

static bool isLan(const IPAddress &ip) {
  return ip[0] == 10 || (ip[0] == 192 && ip[1] == 168) || (ip[0] == 172 && (ip[1] & 0xF0) == 16);
}

// A request to the server: the connection to its address, the name as Host.
static bool openRequest(HTTPClient &http, NetworkClient &tcp, const char *path,
                        int32_t connectMs = UP_HTTP_MS) {
  if (!tcp.connect(server.ip, server.port, connectMs)) return false;
  char uri[180];
  snprintf(uri, sizeof(uri), "%s%s", server.base, path);
  http.setReuse(true);
  return http.begin(tcp, server.host, server.port, uri);
}

// POST /plan once: the HTTP code (200: `todo` is the answer), -1 without
// a connection.
static int askPlan(const UpConfig &c, const char *plan, size_t len, String &todo, int32_t connectMs);

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
    char path[140];
    snprintf(path, sizeof(path), "/f/%s?offset=%lu", name, (unsigned long)from);
    NetworkClient tcp;
    HTTPClient http;
    http.setConnectTimeout(UP_HTTP_MS);
    http.setTimeout(UP_HTTP_MS);
    CardStream s(f, size - from, buf);
    int code = -1;
    if (openRequest(http, tcp, path)) {
      authorise(http, c);
      http.addHeader("Content-Type", "application/octet-stream");
      code = http.sendRequest("PUT", &s, size - from);
    }
    if (code == 200 && !s.failed()) *sent += size - from;
    else err = code == 409 ? "server holds another size" : s.failed() ? "card read failed" : "PUT failed";
    http.end();
  }
  LoggerSdLock lock(SD_LOCK_WEB_MS);
  f.close();
  return err;
}

static int askPlan(const UpConfig &c, const char *plan, size_t len, String &todo, int32_t connectMs) {
  NetworkClient tcp;
  HTTPClient http;
  http.setConnectTimeout(connectMs);
  http.setTimeout(UP_HTTP_MS);
  if (!openRequest(http, tcp, "/plan", connectMs)) return -1;
  authorise(http, c);
  http.addHeader("Content-Type", "text/plain");
  const int code = http.POST((uint8_t *)plan, len);
  if (code == 200) todo = http.getString();
  http.end();
  return code;
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
  bool direct = false;                 // the server reached without the tunnel
  size_t planLen = 0;
  uint16_t onCard = 0;
  String todo;

  if (!buf || !plan) { finish(r, t0, false, "no memory"); goto out; }

  WiFi.mode(WIFI_STA);
  WiFi.begin(c.ssid, c.pass);
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < UP_JOIN_MS) delay(250);
  if (WiFi.status() != WL_CONNECTED) { finish(r, t0, false, "no hotspot"); goto out; }
  Serial.printf("[..] upload: joined the hotspot in %lu ms, RSSI %d\n",
                (unsigned long)(millis() - t0), WiFi.RSSI());
  // The server's name is looked up through the tunnel, once it is up
  // (resolveServer); here only taken apart.
  if (!parseServer(c.url)) { finish(r, t0, false, "the server address is not a URL"); goto out; }

  // The handshake carries a timestamp the router checks against the last
  // one it saw: with no clock (an RTC that lost power, no GNSS fix yet) it
  // is set from NTP first, over the hotspot. esp_sntp directly, not
  // configTime(): that would also set TZ, the gauge's own (settings.tz).
  if (!clock_valid()) {
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    const uint32_t tn = millis();
    while (!clock_valid() && millis() - tn < UP_NTP_MS) delay(200);
    esp_sntp_stop();
    if (!clock_valid()) { finish(r, t0, false, "no clock (NTP)"); goto out; }
    Serial.printf("[..] upload: clock set from NTP in %lu ms\n", (unsigned long)(millis() - tn));
  }

  planLen = listCard(plan, UP_PLAN_MAX, &onCard);
  if (!planLen) { finish(r, t0, false, "card not readable"); goto out; }

  // Straight to the server first, no tunnel: a hotspot sharing the home WiFi
  // (the car starting in the garage, the phone on the home WiFi) puts the
  // gauge inside the home network, where the hotspot's DNS gives the
  // server's LAN address - and where the router's public address, the
  // tunnel's endpoint, does not answer from inside (2026-11-07: "no
  // handshake from the router", every time). A few seconds' try, then the
  // tunnel.
  if (resolveServer("") && isLan(server.ip)) {
    const int code = askPlan(c, plan, planLen, todo, UP_DIRECT_MS);
    Serial.printf("[..] upload: %u logs on the card, the server directly -> %d\n", onCard, code);
    if (code == 401) { finish(r, t0, false, "server refused the token"); goto out; }
    if (code == 200) direct = true;
  }

  if (!direct) {
    {
      IPAddress addr;
      addr.fromString(c.wgAddr);
      char priv[45];
      b64(priv, sizeof(priv), c.wgPriv, sizeof(c.wgPriv));
      Serial.println(F("[..] upload: starting the tunnel"));
      wgUp = wg.begin(addr, priv, c.wgHost, c.wgPeer, c.wgPort);
      memset(priv, 0, sizeof(priv));
    }
    if (!wgUp) { finish(r, t0, false, "tunnel did not start"); goto out; }
    Serial.printf("[..] upload: tunnel up, %u B internal free\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));

    // The server's address, through the tunnel: tried until the handshake
    // has been done and the DNS answers. Which kind it is (not the
    // address) on the console.
    {
      const uint32_t td = millis();
      bool resolved = false;
      while (!(resolved = resolveServer(c.wgDns)) && millis() - td < UP_HANDSHAKE_MS) delay(1000);
      if (!resolved) {
        finish(r, t0, false, wg.is_peer_up() ? "the server's name did not resolve" : "no handshake from the router");
        goto out;
      }
      Serial.printf("[..] upload: the server's name resolved to a %s address in %lu ms\n",
                    isLan(server.ip) ? "private" : "public", (unsigned long)(millis() - td));
    }
  }

  {
    // Through the tunnel the first request may wait for the handshake:
    // tried until it answers.
    const uint32_t tw = millis();
    int code = direct ? 200 : -1;
    while (!direct && millis() - tw < UP_HANDSHAKE_MS) {
      code = askPlan(c, plan, planLen, todo, 5000);
      Serial.printf("[..] upload: %u logs on the card, the server through the tunnel -> %d after %lu ms\n",
                    onCard, code, (unsigned long)(millis() - tw));
      if (code > 0) break;
      delay(2000);
    }
    if (code != 200) {
      // No answer at all: did the router even do the handshake? Four
      // "server not reached" in a row (2026-11-07, hotspot on mobile data)
      // could not say which end it was.
      snprintf(line, sizeof(line), code == 401 ? "server refused the token"
                                   : code > 0  ? "plan answered %d"
                                   : wg.is_peer_up() ? "tunnel up, server not reached"
                                                     : "no handshake from the router", code);
      finish(r, t0, false, line);
      goto out;
    }
  }

  // "name offset" per line: each file's missing part.
  {
    const char *err = nullptr;
    const uint32_t maxMs = c.maxMin * 60000UL;
    int from = 0;
    while (from < (int)todo.length() && millis() - t0 < maxMs) {
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
    // Out of time only with files still to go - the next upload sends them.
    if (err) finish(r, t0, false, line);
    else if (from < (int)todo.length()) finish(r, t0, false, "time limit - the rest goes next time");
    else finish(r, t0, true, "");
  }

out:
  Serial.printf("[..] upload: closing (%s)\n", r.ok ? "done" : r.msg);
  if (wgUp) wg.end();
  Serial.println(F("[..] upload: tunnel closed"));
  WiFi.disconnect(true);
  Serial.println(F("[..] upload: hotspot left"));
  WiFi.mode(WIFI_OFF);
  Serial.println(F("[..] upload: radio off"));
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
  doneThisWake = false;
  triesFailed = 0;
  retryAtMs = 0;
}

bool upload_holds_radio() { return state != UP_IDLE; }
bool upload_busy() { return state != UP_IDLE; }

void upload_task(uint32_t now) {
  static UpConfig taskCfg;            // the task's copy: /save may change cfg
  if (endStepSeen && now > 10000) {
    Serial.printf("[..] upload: the last tunnel close reached step %u of 6\n", endStepSeen);
    endStepSeen = 0;
  }
  if (state == UP_RUNNING) {
    if (!taskDone) return;
    state = UP_IDLE;                  // the radio goes back to the AP
    saveLast();                       // for the app, after the gauge has slept
    if (last.ok || !strncmp(last.msg, "time limit", 10)) {
      doneThisWake = true;
    } else if (++triesFailed >= UP_TRIES) {
      doneThisWake = true;
      Serial.printf("[..] upload: %u tries failed - not again this wake\n", triesFailed);
    } else {
      retryAtMs = now + UP_RETRY_MS;
      Serial.printf("[..] upload: failed (%s) - again in %u s\n", last.msg, UP_RETRY_MS / 1000);
    }
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
  if (doneThisWake) return;
  if (triesFailed && (int32_t)(now - retryAtMs) < 0 && !requested) return;
  // The engine running, the card in and the gauge's own, and nobody on the
  // AP. Or the bench says now - and is told what stops it, once.
  // (No clock is no reason: the task sets it over the hotspot first.)
  // The engine, not just the car talking (2026-11-07): a door opened wakes
  // the bus too, long before the phone joins the car's Bluetooth and turns
  // its hotspot on - each such try held the AP down a minute for nothing.
  const bool running = podstate().car == PS_CAR_RUNNING;
  if (!running && !requested) return;
  const char *why = !configured()                          ? "not set up (the app's Upload card)"
                    : !logger_card_ok() || logger_card_shared() ? "no card, or it is lent to a PC"
                    : web_clients() > 0                     ? "a phone is on the gauge's WiFi"
                                                            : nullptr;
  if (why) {
    if (requested) Serial.printf("[..] upload: not now - %s\n", why);
    requested = false;
    return;
  }
  requested = false;
  state = UP_WAIT_AP;
  Serial.println(F("[..] upload: the engine runs - the radio is the upload's"));
}
