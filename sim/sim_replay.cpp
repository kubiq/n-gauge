#include "sim_replay.h"

#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>

#include "Arduino.h"
#include "gps.h"
#include "obd.h"

Replay replay;

static float num(const std::vector<std::string> &f, int i) {
  if (i < 0 || i >= (int)f.size() || f[i].empty()) return NAN;
  char *end;
  const float v = std::strtof(f[i].c_str(), &end);
  return end == f[i].c_str() ? NAN : v;
}

static std::vector<std::string> split(const std::string &line) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : line) {
    if (c == ',') { out.push_back(cur); cur.clear(); }
    else if (c != '\r') cur += c;
  }
  out.push_back(cur);
  return out;
}

// "2026-10-14T10:42:24Z" or with fractions -> epoch ms; 0 if not a time.
static long long isoMs(const std::string &s) {
  int y, mo, d, h, mi;
  double sec;
  if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%lfZ", &y, &mo, &d, &h, &mi, &sec) != 6) return 0;
  std::tm tm{};
  tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d;
  tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = 0;
  return (long long)timegm(&tm) * 1000 + (long long)std::llround(sec * 1000.0);
}

bool Replay::loadCsv(const std::string &path) {
  std::ifstream in(path);
  if (!in) return false;
  std::string line;
  if (!std::getline(in, line)) return false;
  const auto head = split(line);
  std::map<std::string, int> col;
  for (size_t i = 0; i < head.size(); i++) col[head[i]] = (int)i;
  auto c = [&](const char *n) { auto it = col.find(n); return it == col.end() ? -1 : it->second; };
  const int iMs = c("ms"), iUtc = c("utc");
  if (iMs < 0) return false;
  long long first = -1, zero = LLONG_MIN;
  while (std::getline(in, line)) {
    const auto f = split(line);
    if ((int)f.size() <= iMs || f[iMs].empty()) continue;
    const long long ms = std::atoll(f[iMs].c_str());
    if (first < 0) first = ms;
    if (ms < first) continue;   // a restart mid-log: left out, as the app does
    if (iUtc >= 0 && (int)f.size() > iUtc) {
      const long long u = isoMs(f[iUtc]);
      if (u && u - ms > zero) zero = u - ms;   // utc is whole seconds: the largest is the closest
    }
    ReplayRow r{};
    r.t = (uint32_t)(ms - first);
    r.oilC = num(f, c("oilC")); r.coolantC = num(f, c("coolantC")); r.boostBar = num(f, c("boostBar"));
    r.kmh = num(f, c("obdKmh")); r.rpm = num(f, c("rpm")); r.pedalRaw = num(f, c("pedalRaw"));
    r.absLoad = num(f, c("absLoadPct")); r.timing = num(f, c("timingDeg")); r.iat = num(f, c("iatC"));
    r.volts = num(f, c("volts")); r.baro = num(f, c("baroKpa"));
    for (int w = 0; w < 4; w++) {
      char b[8];
      std::snprintf(b, sizeof(b), "t%dbar", w); r.tyreBar[w] = num(f, c(b));
      std::snprintf(b, sizeof(b), "t%dc", w);   r.tyreC[w]   = num(f, c(b));
    }
    r.gx = num(f, c("gx")); r.gy = num(f, c("gy")); r.gz = num(f, c("gz"));
    rows.push_back(r);
  }
  if (zero != LLONG_MIN && first >= 0) startUtcMs = zero + first;
  if (!rows.empty()) lengthMs = rows.back().t;
  return !rows.empty();
}

static std::vector<ReplayFix> gpxRaw;
static std::vector<long long> gpxUtc;

bool Replay::loadGpx(const std::string &path) {
  FILE *f = std::fopen(path.c_str(), "r");
  if (!f) return false;
  char line[1024];
  while (std::fgets(line, sizeof(line), f)) {
    const char *p = std::strstr(line, "<trkpt lat=\"");
    double lat, lon;
    if (!p || std::sscanf(p, "<trkpt lat=\"%lf\" lon=\"%lf\"", &lat, &lon) != 2) continue;
    const char *tm = std::strstr(line, "<time>");
    if (!tm) continue;
    const long long u = isoMs(std::string(tm + 6, 32));
    if (!u) continue;
    ReplayFix x{};
    x.lat1e7 = (int32_t)std::llround(lat * 1e7);
    x.lon1e7 = (int32_t)std::llround(lon * 1e7);
    x.msOfDay = (uint32_t)(u % 86400000LL);
    const char *e = std::strstr(line, "<ele>"), *sa = std::strstr(line, "<sat>"), *hd = std::strstr(line, "<hdop>");
    const char *sp = std::strstr(line, "speed>");
    x.altM = e ? std::atof(e + 5) : 0.0f;
    x.sats = sa ? std::atoi(sa + 5) : 12;
    x.hdop = hd ? (float)std::atof(hd + 6) : 1.0f;
    x.kmh = sp ? (float)std::atof(sp + 6) * 3.6f : NAN;
    gpxRaw.push_back(x);
    gpxUtc.push_back(u);
  }
  std::fclose(f);
  return !gpxRaw.empty();
}

void Replay::finish() {
  // The GPX on the CSV's clock; a GPX alone starts the replay itself.
  if (!startUtcMs && !gpxUtc.empty()) startUtcMs = gpxUtc.front();
  for (size_t i = 0; i < gpxRaw.size(); i++) {
    const long long t = gpxUtc[i] - startUtcMs;
    if (t < 0) continue;
    gpxRaw[i].t = (uint32_t)t;
    fixes.push_back(gpxRaw[i]);
  }
  if (!fixes.empty() && fixes.back().t > lengthMs) lengthMs = fixes.back().t;
}

// ---- feeding ---------------------------------------------------------------

static size_t nextRow = 0, nextFix = 0;
static uint32_t lastFixT = 0;
float simImu[3] = {0.0f, 0.0f, 1.0f};

void replay_rewind() {
  nextRow = nextFix = 0;
}

static void apply(const ReplayRow &r) {
  const uint32_t now = millis();
  bool car = false;
  auto set = [&](float v, auto apply) { if (!std::isnan(v)) { apply(v); car = true; } };
  set(r.oilC, [&](float v) { obd.oilTempC = (int)std::lround(v); obd.haveOil = true; obd.oilMs = now; });
  set(r.coolantC, [&](float v) { obd.coolantC = (int)std::lround(v); obd.haveCoolant = true; obd.coolantMs = now; });
  set(r.boostBar, [&](float v) { obd.boostBar = v; obd.haveBoost = true; obd.boostMs = now; });
  set(r.kmh, [&](float v) { obd.speedKmh = (int)v; obd.haveSpeed = true; obd.speedMs = now; });
  set(r.rpm, [&](float v) { obd.rpm = (int)v; obd.haveRpm = true; obd.rpmMs = now; });
  set(r.pedalRaw, [&](float v) { obd.pedalRaw = (uint8_t)v; obd.havePedal = true; obd.pedalMs = now; });
  set(r.absLoad, [&](float v) { obd.absLoadPct = v; obd.haveAbsLoad = true; obd.absLoadMs = now; });
  set(r.timing, [&](float v) { obd.timingDeg = v; obd.haveTiming = true; obd.timingMs = now; });
  set(r.iat, [&](float v) { obd.iatC = (int)v; obd.haveIat = true; obd.iatMs = now; });
  set(r.volts, [&](float v) { obd.volts = v; obd.haveVolts = true; obd.voltsMs = now; });
  if (!std::isnan(r.baro)) { obd.baroKpa = (int)r.baro; obd.haveBaro = true; }
  for (int w = 0; w < 4; w++) {
    if (std::isnan(r.tyreBar[w])) continue;
    obd.tyreBar[w] = r.tyreBar[w];
    obd.tyreTempC[w] = std::isnan(r.tyreC[w]) ? 0 : (int)r.tyreC[w];
    obd.haveTyre[w] = true;
    obd.tyreMs[w] = now;
  }
  // "The car is on": any frame but the TPMS module's (obd.cpp).
  if (car) obd.lastFrameMs = now;
  if (!std::isnan(r.gx) && !std::isnan(r.gy) && !std::isnan(r.gz)) {
    simImu[0] = r.gx; simImu[1] = r.gy; simImu[2] = r.gz;
  }
}

static GpsFix currentFix;
static bool haveCurrent = false;

void replay_feed(uint32_t t) {
  while (nextRow < replay.rows.size() && replay.rows[nextRow].t <= t) apply(replay.rows[nextRow++]);
  bool moved = false;
  while (nextFix < replay.fixes.size() && replay.fixes[nextFix].t <= t) {
    const ReplayFix &x = replay.fixes[nextFix++];
    gps.lat1e7 = x.lat1e7;
    gps.lon1e7 = x.lon1e7;
    gps.altM = x.altM;
    gps.sats = (uint8_t)x.sats;
    gps.hdop = x.hdop;
    if (!std::isnan(x.kmh)) gps.speedKmh = x.kmh;
    currentFix = GpsFix{x.lat1e7, x.lon1e7, x.altM, x.msOfDay, true, (uint32_t)nextFix};
    haveCurrent = true;
    lastFixT = x.t;
    moved = true;
  }
  (void)moved;
  // The module is there whenever the drive has a track; a fix is current
  // while points keep coming (a tunnel is a gap in them).
  gps.linkUp = !replay.fixes.empty();
  gps.haveFix = haveCurrent && t - lastFixT < 2000;
  gps.timeSet = gps.haveFix;
}

bool gps_read_fix(GpsFix *out) {
  if (!haveCurrent) return false;
  *out = currentFix;
  return true;
}

bool gps_read_position(int32_t *lat1e7, int32_t *lon1e7, float *altM) {
  if (!gps.haveFix) return false;
  *lat1e7 = gps.lat1e7;
  *lon1e7 = gps.lon1e7;
  *altM = gps.altM;
  return true;
}
