// ============================================================================
// bench.cpp  —  the desk tools. See bench.h; none of it is in the car's build.
// ============================================================================
#include "bench.h"

#if BENCH_CONSOLE || FRAME_STATS || WOM_BENCH
#include <Arduino.h>
#include "esp_heap_caps.h"   // 'h'
#include "esp_task_wdt.h"    // 'x' waits 15 s
#include "bsp.h"
#include "obd.h"
#include "gps.h"
#include "rtc.h"
#include "settings.h"
#include "webapi.h"
#include "logger.h"
#include "ui.h"

#if BENCH_CONSOLE
static bool benchSynth = false;
static bool benchNoRim = false;
static int  benchApKmh = -1;

bool bench_synth()  { return benchSynth; }
bool bench_no_rim() { return benchNoRim; }
int  bench_ap_kmh() { return benchApKmh; }

// One reader, taken once per pass. Each handler used to read for itself, and
// the 'b' check consumed whatever byte was waiting - so an 's' landing
// mid-pass was silently eaten.
static void benchCommands(uint32_t now) {
  const int cmd = Serial.available() ? Serial.read() : -1;
  // 'c' pretends a CAN frame arrived (the drive log opens on the first one),
  // 't<epoch>' sets the clock like the phone page does - together they let
  // the logger's drive-start and rename paths be exercised on the desk.
  if (cmd == 'c' && obd.lastFrameMs == 0) obd.lastFrameMs = now ? now : 1;
  // 'f' injects one set of readings stamped now, then nothing refreshes them -
  // a dropout on demand. OBD values go stale after OBD_STALE_MS; tyres are
  // stamped nearly TPMS_STALE_MS old so they follow a few seconds later.
  if (cmd == 'f') {
    const uint32_t t = millis();
    obd.oilTempC = 95;  obd.oilRaw = 192; obd.oilMs = t;     obd.haveOil = true;
    obd.coolantC = 90;  obd.coolantMs = t;                   obd.haveCoolant = true;
    obd.boostBar = 1.23f; obd.boostMs = t;                   obd.haveBoost = true;
    obd.speedKmh = 50;  obd.speedMs = t;                     obd.haveSpeed = true;
    obd.absLoadPct = 200; obd.absLoadMs = t;                 obd.haveAbsLoad = true;
    obd.pedalRaw = 120; obd.pedalMs = t;                     obd.havePedal = true;
    // One tyre per case, so a single look shows them all: normal, normal,
    // low, high - the last two should read red.
    static const float bar[4]  = { 2.5f, 2.4f, 1.7f, 3.2f };
    static const int   temp[4] = { 31, 30, 28, 33 };
    for (int i = 0; i < 4; i++) {
      obd.tyreBar[i] = bar[i]; obd.tyreTempC[i] = temp[i];
      obd.tyreMs[i] = t - (TPMS_STALE_MS - 3000);            obd.haveTyre[i] = true;
    }
    Serial.println(F("[..] bench: fake readings injected, stale in ~3-5 s"));
  }
  // 'd' prints the tail of the drive log to check what the CSV recorded.
  if (cmd == 'd') logger_dump_tail(6000);
  // 'h': internal RAM now, the least it has been since boot, and the largest
  // block - against AP_MIN_INTERNAL_HEAP, what decides whether the AP starts.
  if (cmd == 'h') {
    Serial.printf("[..] internal RAM: %u KB free, %u KB at the least, largest block %u KB; AP %s, refused %lu\n",
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024),
                  (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024),
                  (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024),
                  web_is_up() ? "up" : "down", (unsigned long)web_ap_refused());
  }
  // 'v<kmh>' - see bench_ap_kmh().
  if (cmd == 'v') {
    benchApKmh = (int)Serial.parseInt();
    if (benchApKmh < 0) benchApKmh = -1;
    Serial.printf("[..] bench: AP logic sees %s\n",
                  benchApKmh < 0 ? "the real GNSS" :
                  benchApKmh == 0 ? "no fix" : "a fix at the given speed");
  }
  if (cmd == 't') {
    const long e = Serial.parseInt();
    Serial.printf("[..] bench: set clock to %ld -> %s\n", e,
                  rtc_set_from_epoch(e) ? "ok" : "rejected");
  }
  // 'k<64 hex>' / 'koff' sets the log encryption key as the phone app does,
  // under the same lock - for checking the encrypted logs on the desk.
  if (cmd == 'k') {
    String hex = Serial.readStringUntil('\n');
    hex.trim();
    LoggerSdLock lock;
    Serial.printf("[..] bench: log key %s -> %s\n", hex.c_str(),
                  settings_set_log_key(hex.c_str()) ? "ok" : "rejected");
  }
  if (cmd == 'r') benchNoRim = !benchNoRim;
  if (cmd == 's') benchSynth = !benchSynth;
  // 'b' runs the render benchmark.
  if (cmd == 'b') ui_bench();
  // 'p': does the GPS take input - NMEA, UBX, neither? See gps_bench_probe().
  if (cmd == 'p') gps_bench_probe();
  // 'x' holds GPS_TX_GPIO low as a plain output for 15 s, then restarts: with
  // a meter on the GPS module's RX pin, 0 V proves the ESP drives that line,
  // 3.3 V that it does not (the module's own pull-up is what reads). An idle
  // UART line is high either way, so a steady 3.3 V says nothing by itself
  // (2026-10-21: the carrier's GPS never ACKed its configuration).
  if (cmd == 'x') {
    Serial.printf("[..] bench: GPIO%d held LOW for 15 s - measure the GPS module's RX pin now\n",
                  GPS_TX_GPIO);
    pinMode(GPS_TX_GPIO, OUTPUT);
    digitalWrite(GPS_TX_GPIO, LOW);
    for (int i = 0; i < 15; i++) { delay(1000); esp_task_wdt_reset(); }
    Serial.println(F("[..] bench: done - restarting to give the pin back to the GPS UART"));
    Serial.flush();
    ESP.restart();
  }
}
#endif

#if FRAME_STATS
// How often each OBD value actually arrives - the check for request pacing
// on the car. Counts changes of the *Ms stamps.
static void obdRates(uint32_t now) {
  static uint32_t since = 0, lastB = 0, lastO = 0, lastC = 0, nB = 0, nO = 0, nC = 0;
  if (obd.boostMs != lastB)   { lastB = obd.boostMs;   nB++; }
  if (obd.oilMs != lastO)     { lastO = obd.oilMs;     nO++; }
  if (obd.coolantMs != lastC) { lastC = obd.coolantMs; nC++; }
  if (now - since >= 2000) {
    if (obd.lastFrameMs)
      Serial.printf("[OBD] updates/s: boost %.1f, oil %.1f, coolant %.1f\n",
                    nB * 1000.0f / (now - since), nO * 1000.0f / (now - since),
                    nC * 1000.0f / (now - since));
    nB = nO = nC = 0; since = now;
  }
}
#endif

#if WOM_BENCH
// See WOM_BENCH in config.h. Polled, not an ISR: the point is to see every
// INT2 change with its timing and the sensor's own status alongside.
static void womBench(uint32_t now) {
  static bool    armed = false;
  static uint8_t thr = WOM_BENCH_MG;
  static int     lastLevel = -1;
  static uint32_t events = 0, armedAt = 0;
  if (!armed) {
    pinMode(21, INPUT);
    const bool ok = bsp_imu_wom_arm(thr);
    armed = true;
    armedAt = now;
    lastLevel = digitalRead(21);
    Serial.printf("[WOM] armed at %u mg: %s, INT2 idle level %d\n",
                  thr, ok ? "ok" : "FAILED", lastLevel);
  }
  const int level = digitalRead(21);
  if (level != lastLevel) {
    const uint8_t st = bsp_imu_status1();
    events++;
    Serial.printf("[WOM] #%lu INT2 %d->%d at %lu.%03lus (thr %u mg, STATUS1 0x%02X%s)\n",
                  (unsigned long)events, lastLevel, level,
                  (unsigned long)((now - armedAt) / 1000), (unsigned long)((now - armedAt) % 1000),
                  thr, st, (st & 0x04) ? " motion" : "");
    lastLevel = level;
  }
  // A number + Enter re-arms at that threshold.
  if (Serial.available()) {
    const long v = Serial.parseInt();
    while (Serial.available()) Serial.read();
    if (v >= 1 && v <= 255) { thr = (uint8_t)v; armed = false; events = 0; }
    else Serial.println(F("[WOM] threshold must be 1-255 mg"));
  }
}
#endif

void bench_task(uint32_t now) {
  (void)now;
#if WOM_BENCH
  womBench(now);      // reads serial itself: a number re-arms it
#elif BENCH_CONSOLE
  benchCommands(now);
#endif
#if FRAME_STATS
  obdRates(now);
#endif
}
#endif
