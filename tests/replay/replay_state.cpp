// Replays a recorded drive CSV through the gauge's state (podstate.h), row by
// row at the recorded uptime - to see on a desk what it makes of a real drive:
// when the car goes off, on, running; when the drive starts and ends; when it
// would want the WiFi, thin the log, or allow sleep and what holds it off.
//
//   tests/replay/replay_state.sh data/drive_2026-10-22_21-00_59a9bb.csv
//
// Beside it, what the gauge really did with the WiFi (the logged apUp), so a
// change of behaviour shows before it is on the gauge.
//
// From a CSV, a frame from the car is a row with any of its readings - or,
// in logs with it, `car` above 0. The readings go empty OBD_STALE_MS after
// the car stops answering, so "off" lands that much later than on the gauge.
// No phone, no touch, no USB: a log does not say.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "podstate.h"
#include "config.h"

static const PsCfg CFG = {
  OBD_STALE_MS, ENGINE_RUNNING_RPM, MOVING_OBD_KMH, SLEEP_HOLD_MOVING_KMH,
  AP_DRIVE_ON_KMH, AP_DRIVE_ON_MS, AP_DRIVE_OFF_KMH, AP_DRIVE_OFF_MS,
  LOG_IDLE_AFTER_MS, BUS_IDLE_SLEEP_MS, COLD_BOOT_AWAKE_MS,
  CAN_MISSING_GRACE_MS, GPS_MISSING_GRACE_MS,
};

static std::vector<std::string> split(const std::string &line) {
  std::vector<std::string> out;
  size_t a = 0;
  for (;;) {
    size_t b = line.find(',', a);
    out.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
    if (b == std::string::npos) break;
    a = b + 1;
  }
  if (!out.empty() && !out.back().empty() && out.back().back() == '\r') out.back().pop_back();
  return out;
}

static void awakeText(uint8_t a, char *out, size_t n) {
  static const char *names[] = {"waking", "usb", "phone", "user", "moving", "car", "boot"};
  out[0] = 0;
  for (int i = 0; i < 7; i++) {
    if (!(a & (1u << i))) continue;
    if (out[0]) strncat(out, "+", n - strlen(out) - 1);
    strncat(out, names[i], n - strlen(out) - 1);
  }
  if (!out[0]) snprintf(out, n, "-");
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s drive.csv\n", argv[0]);
    return 2;
  }
  FILE *fp = fopen(argv[1], "r");
  if (!fp) { perror(argv[1]); return 1; }
  std::string line;
  char buf[8192];
  std::vector<std::string> head;
  if (fgets(buf, sizeof buf, fp)) head = split(std::string(buf, strcspn(buf, "\n")));
  auto col = [&](const char *n) {
    for (size_t i = 0; i < head.size(); i++) if (head[i] == n) return (int)i;
    return -1;
  };
  const int iUtc = col("utc"), iMs = col("ms"), iRpm = col("rpm"), iKmh = col("obdKmh");
  const int iGps = col("speedKmh"), iFix = col("gpsFix"), iSats = col("sats");
  const int iAp = col("apUp"), iCar = col("car");
  const int carCols[] = {col("oilC"), col("coolantC"), iRpm, iKmh};
  if (iMs < 0) { fprintf(stderr, "no ms column\n"); return 1; }

  PodState s = {};
  PsFacts f = {};
  f.canLinkUp = true;
  f.gpsLink = true;
  f.card = PS_CARD_OK;
  f.clockValid = true;
  bool first = true;
  PsCar lastCar = PS_CAR_OFF;
  PsMotion lastMotion = PS_STILL;
  bool lastWifi = false, lastSleep = false;
  PsLogRate lastLog = PS_LOG_FULL;
  uint8_t lastAwake = 0;
  int lastAp = -1;
  uint32_t lastMs = 0;
  printf("%-8s  %-8s %-8s %-6s %-5s %-6s %-22s %s\n",
         "utc", "car", "motion", "wifi", "log", "sleep", "awake because", "logged apUp");
  while (fgets(buf, sizeof buf, fp)) {
    std::vector<std::string> v = split(std::string(buf, strcspn(buf, "\n")));
    auto has = [&](int i) { return i >= 0 && i < (int)v.size() && !v[i].empty(); };
    if (!has(iMs)) continue;
    const uint32_t ms = (uint32_t)strtoul(v[iMs].c_str(), nullptr, 10);
    if (!first && ms < lastMs) {           // the gauge restarted: a new boot
      printf("--- restart ---\n");
      s = PodState{};
      f.lastFrameMs = 0;
    }
    lastMs = ms;
    f.now = ms;
    bool talking = false;
    if (has(iCar)) talking = atoi(v[iCar].c_str()) > 0;
    else for (int i : carCols) talking |= has(i);
    if (talking) f.lastFrameMs = ms ? ms : 1;
    f.rpmFresh = has(iRpm);
    f.rpm = f.rpmFresh ? (int16_t)atoi(v[iRpm].c_str()) : 0;
    f.obdKmhFresh = has(iKmh);
    f.obdKmh = f.obdKmhFresh ? (int16_t)atoi(v[iKmh].c_str()) : 0;
    f.gpsFix = has(iFix) && atoi(v[iFix].c_str()) == 1 && has(iGps);
    f.gpsKmh = f.gpsFix ? (float)atof(v[iGps].c_str()) : 0;
    f.sats = has(iSats) ? (uint8_t)atoi(v[iSats].c_str()) : 0;
    podstate_step(&s, f, CFG);

    const int ap = has(iAp) ? atoi(v[iAp].c_str()) : -1;
    if (first || s.car != lastCar || s.motion != lastMotion || s.wifiWanted != lastWifi ||
        s.logRate != lastLog || s.sleepOk != lastSleep || s.awakeBecause != lastAwake || ap != lastAp) {
      char awake[64];
      awakeText(s.awakeBecause, awake, sizeof awake);
      const char *utc = has(iUtc) && v[iUtc].size() >= 19 ? v[iUtc].c_str() + 11 : "--:--:--";
      char apText[8];
      snprintf(apText, sizeof apText, "%s", ap < 0 ? "?" : ap ? "up" : "down");
      printf("%.8s  %-8s %-8s %-6s %-5s %-6s %-22s %s%s   (%.0f km/h %s)\n", utc,
             ps_car_name(s.car), ps_motion_name(s.motion), s.wifiWanted ? "want" : "off",
             s.logRate == PS_LOG_THIN ? "thin" : "full", s.sleepOk ? "ok" : "held", awake, apText,
             (ap >= 0 && (ap == 1) != s.wifiWanted) ? "  <- differs" : "",
             s.kmh, s.speedFrom == PS_SPEED_OBD ? "obd" : s.speedFrom == PS_SPEED_GPS ? "gps" : "-");
      lastCar = s.car; lastMotion = s.motion; lastWifi = s.wifiWanted; lastLog = s.logRate;
      lastSleep = s.sleepOk; lastAwake = s.awakeBecause; lastAp = ap;
      first = false;
    }
  }
  fclose(fp);
  return 0;
}
