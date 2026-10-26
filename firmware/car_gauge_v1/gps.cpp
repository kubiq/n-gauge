// ============================================================================
// gps.cpp  —  NEO-M9N NMEA reader. Parsing is TinyGPSPlus; this file is just
//             the UART plumbing plus a baud hunt.
// ============================================================================
#include "gps.h"
#include "config.h"
#include "calc.h"       // calc_timegm; timegm() is absent from this newlib
#include "rtc.h"        // clock_valid(): step a clock not yet set, slew a set one
#include "taskwdt.h"
#include <Arduino.h>
#include <esp_heap_caps.h>  // the fix queue, in PSRAM
#include <TinyGPSPlus.h>
#include <sys/time.h>
#include <time.h>

GpsState gps;

static TinyGPSPlus     nmea;
static HardwareSerial  gpsSerial(1);          // UART1; UART0 is the USB console

// u-blox M9 ships at 38400 on UART1, but plenty of these breakout boards are
// flashed at 9600 (the old M8 default) and some at 115200. Rather than make
// the user guess with no serial monitor in the car, rotate through the
// candidates until real NMEA decodes.
// Order matters for startup latency. 38400 is the module's cold default;
// 115200 is where our own config leaves it, and it survives an ESP reboot
// because resetting the ESP does not power-cycle the module. Those two are
// therefore by far the likeliest, so try them first - otherwise a warm boot
// wastes ~5 s with no GPS while stepping through 9600.
static const uint32_t GPS_BAUDS[] = { 38400, 115200, 9600, 57600, 4800 };
static uint8_t  baudIdx    = 0;
static uint32_t baudTryMs  = 0;
static bool     configTried = false;   // one attempt only, ACK or not

static void gpsTask(void *arg);        // defined below, spawned from gps_init()

// Seqlock over the position fields (lat1e7, lon1e7, altM). Each field on its
// own is a 32-bit atomic store, but the SET is not: once the GNSS moved onto
// core 0, a reader on core 1 could pair lat from one fix with lon from the
// next, or read the position between gps.haveFix going true and the first
// coordinates landing - which accepted (0,0) as a real point. Odd = a write is
// in progress. 0 = no position has ever been published.
static volatile uint32_t posSeq = 0;

// Park handshake between goToSleep() (core 1) and the GNSS task (core 0) - see
// gps_park(). Declared up here, not beside it, because the UBX wait loops
// below have to see parkReq: they can block for seconds, and a park request
// that sits unanswered past GPS_PARK_ACK_MS lets the board sleep with the
// receiver still awake at ~17 mA.
static volatile bool parkReq  = false;
static volatile bool parkDone = false;

// Fix time, published under the same seqlock as the position so the two always
// come from the same fix. Private: read it through gps_read_fix().
static uint32_t posFixMs  = 0;
static bool     posTimeOk = false;

bool gps_read_fix(GpsFix *out) {
  // Retried for a while, not a fixed number of times. The writer holds the
  // seqlock for a few instructions - but it runs at priority 4 on core 0 and
  // the CAN task (5) can pre-empt it mid-update, for as long as a CAN frame
  // takes to handle. Eight tight retries from core 1 spanned a microsecond or
  // two and gave up on a perfectly good fix; GPS_READ_WAIT_US covers the
  // pre-emption, with a short pause between tries so they do not hammer.
  const uint32_t t0 = micros();
  for (bool first = true;; first = false) {
    if (!first) {
      if (micros() - t0 >= GPS_READ_WAIT_US) break;
      delayMicroseconds(5);
    }
    const uint32_t s1 = posSeq;
    if (s1 == 0) return false;                  // nothing published yet
    if (s1 & 1) continue;                       // writer mid-update; retry
    __sync_synchronize();
    GpsFix f;
    f.lat1e7 = gps.lat1e7;
    f.lon1e7 = gps.lon1e7;
    f.altM   = gps.altM;
    f.fixMs  = posFixMs;
    f.timeOk = posTimeOk;
    f.seq    = s1;
    __sync_synchronize();
    if (posSeq != s1) continue;                 // changed under us; retry
    *out = f;
    return true;
  }
  // Still mid-update after all that: something is badly wrong. Refusing is
  // safer than returning a position we know might be torn.
  return false;
}

// ---- every fix, queued (gps.h, fixring.h) -----------------------------------
// 256: each fix is published twice (RMC and GGA), ~50 a second - ~5 s, four
// times the longest card stall seen. In PSRAM (~45 KB), not the internal RAM
// the AP needs; allocated in gps_init().
static constexpr uint32_t GPS_RING_N = 256;
static FixRing<GpsSample, GPS_RING_N> ring = {nullptr, 0};
static volatile GpsSampleHook sampleHook = nullptr;

void gps_set_sample_hook(GpsSampleHook h) { sampleHook = h; }

// A sample of the fix just published, with what is true at it now.
static void sampleNow(GpsSample *s, const GpsFix &f) {
  s->fix = f;
  s->speedKmh = gps.speedKmh;
  s->courseDeg = gps.courseDeg;
  s->hdop = gps.hdop;
  s->sats = gps.sats;
  const GpsSampleHook h = sampleHook;
  if (h) h(s);
}

bool gps_next_sample(FixCursor *c, GpsSample *out, uint32_t *lost) {
  if (ring.slots) return fixring_next(&ring, c, out, lost);
  // No ring: the latest fix, once - what the logs read before it.
  GpsFix f;
  if (!gps.haveFix || !gps_read_fix(&f) || (c->started && f.seq == c->next)) return false;
  c->started = true;
  c->next = f.seq;
  sampleNow(out, f);
  return true;
}

bool gps_read_position(int32_t *lat, int32_t *lon, float *alt) {
  GpsFix f;
  if (!gps_read_fix(&f)) return false;
  *lat = f.lat1e7; *lon = f.lon1e7; if (alt) *alt = f.altM;
  return true;
}

static void openPort(uint8_t idx) {
  gps.baud = GPS_BAUDS[idx];
  gpsSerial.begin(gps.baud, SERIAL_8N1, GPS_RX_GPIO, GPS_TX_GPIO);
  baudTryMs = millis();
}

// ---- UBX configuration ----------------------------------------------------
// Frame: B5 62 | class | id | len(LE u2) | payload | ckA ckB, where the
// Fletcher-8 checksum runs from `class` to the end of the payload.
static void ubxSend(uint8_t cls, uint8_t id, const uint8_t *payload, uint16_t len) {
  uint8_t hdr[6] = { 0xB5, 0x62, cls, id, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };
  uint8_t ckA = 0, ckB = 0;
  for (int i = 2; i < 6; i++) { ckA += hdr[i]; ckB += ckA; }
  for (uint16_t i = 0; i < len; i++) { ckA += payload[i]; ckB += ckA; }
  gpsSerial.write(hdr, sizeof(hdr));
  if (len) gpsSerial.write(payload, len);
  gpsSerial.write(ckA);
  gpsSerial.write(ckB);
  gpsSerial.flush();
}

// Waits for a specific UBX frame, returning its payload length or -1.
// Interleaved NMEA is discarded, which is fine - this only runs at startup.
static int ubxAwaitFrame(uint8_t cls, uint8_t id, uint8_t *out, uint16_t outMax,
                         uint32_t timeoutMs) {
  enum { SYNC1, SYNC2, CLS, ID, LEN1, LEN2, BODY } st = SYNC1;
  uint8_t  gotCls = 0, gotId = 0;
  uint16_t len = 0, got = 0;
  const uint32_t t0 = millis();

  while (millis() - t0 < timeoutMs) {
    // Give up at once if the board wants to sleep. These waits can run for
    // seconds, and gps_park() only waits GPS_PARK_ACK_MS for the task to get
    // back to its loop and send the PMREQ - otherwise the board sleeps with
    // the receiver awake. Aborting is safe: gpsConfigure() treats a missing
    // ACK as "not configured" and returns BEFORE the baud change, so the
    // module and the ESP cannot end up on different rates.
    if (parkReq) return -1;

    // A plain delay is correct again now that this runs on its own core.
    //
    // It briefly called bsp_ui_pump() instead, because these waits ran from
    // loop() and between MON-VER, the CFG-VALSET ACK and the NAV-PVT poll they
    // can block ~3.5 s when the module is slow - which froze the boot
    // animation mid-sweep, since LVGL only advances when lv_timer_handler()
    // runs. On core 0 that cannot happen, and calling into LVGL from here
    // would now be actively wrong: lv_timer_handler() is not re-entrant and
    // must never run on two tasks.
    if (!gpsSerial.available()) { delay(1); continue; }
    const uint8_t c = gpsSerial.read();
    switch (st) {
      case SYNC1: st = (c == 0xB5) ? SYNC2 : SYNC1; break;
      case SYNC2: st = (c == 0x62) ? CLS : (c == 0xB5 ? SYNC2 : SYNC1); break;
      case CLS:   gotCls = c; st = ID; break;
      case ID:    gotId = c; st = LEN1; break;
      case LEN1:  len = c; st = LEN2; break;
      case LEN2:  len |= (uint16_t)c << 8; got = 0;
                  st = len ? BODY : SYNC1;
                  if (!len && gotCls == cls && gotId == id) return 0;
                  break;
      case BODY:
        if (got < outMax) out[got] = c;
        if (++got >= len) {
          st = SYNC1;
          if (gotCls == cls && gotId == id) return len < outMax ? len : outMax;
        }
        break;
    }
  }
  return -1;
}

// ACK-ACK is class 0x05 id 0x01, ACK-NAK is 0x05/0x00. Distinguishing them
// matters: "rejected" and "never arrived" point at completely different
// problems (wrong keys vs. no TX wire).
enum UbxAck { ACK_NONE, ACK_OK, ACK_NAK };
static UbxAck ubxAwaitAck(uint32_t timeoutMs) {
  uint8_t p[8];
  const uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    // Must check here too: once parkReq is set ubxAwaitFrame returns at once,
    // so without this the loop would spin flat out for the rest of timeoutMs.
    if (parkReq) return ACK_NONE;
    if (ubxAwaitFrame(0x05, 0x01, p, sizeof(p), 200) >= 0) return ACK_OK;
    if (ubxAwaitFrame(0x05, 0x00, p, sizeof(p), 1)   >= 0) return ACK_NAK;
  }
  return ACK_NONE;
}

// Ask the module what time it thinks it is, BEFORE it has a fix.
//
// The module has a backup cell on V_BCKP (measured 2.95 V), so its internal
// RTC keeps running through a full power removal - the one case where both the
// ESP32's clock and the PCF85063 (which has no cell) give up. That makes this
// module the only actually battery-backed clock in the system.
//
// The reason it is not free: u-blox blanks the NMEA time fields until time is
// fully resolved, which in practice means a fix. So the RTC time is there but
// unpublished over NMEA - which is why the clock took ~58 s to appear on the
// bench despite the cell. UBX-NAV-PVT exposes it with explicit validity bits
// instead, so we can tell "known to about a second" from "not known".
//
// ⚠ MEASURED 2026-09-24: on this breakout it does NOT work. The module reports
// validDate=0, validTime=0, tAcc=0xFFFFFFFF and the firmware-default date
// 2020-08-02, cross-checked against NAV-TIMEUTC which agreed (its valid byte
// read 0xF0: no valid bits, plus utcStandard=0xF "unknown" in bits 4-7, which
// is why that byte looks alarming rather than misaligned).
//
// Cause is narrowed to the backup cell not actually backing anything. 2.95 V
// measured across the cell-looking part proves only that it holds charge, not
// that it reaches V_BCKP - on these clones it is often fitted to the footprint
// unconnected, or is a supercap that self-discharges.
//
// Specifically NOT the cause: the module's main rail being cycled by the ESP.
// The time it reports counts up from its own power-on, and it read 00:06:13 ->
// 00:08:06 -> 00:08:16 across successive ESP resets before dropping to
// 00:00:51 only after a real power cycle. So it runs straight through our
// resets, and bsp_init's AXP2101 setup is not disturbing it. That also means
// gps_park()/the wake burst are not losing the module's ephemeris.
//
// The code is left in place because it costs one UBX poll and starts working by
// itself the moment the backup does - and the NAV-PVT line is worth having in
// the boot log regardless: it reports fixType and the validity bits, which is
// the only place "no fix yet" is distinguishable from "module knows nothing".
//
// Deliberately does NOT set gps.timeSet. That flag means "set from a resolved
// fix", and two things depend on the distinction: the NMEA path still corrects
// the clock precisely at first fix, and rtc_task() waits for that before
// writing the PCF85063 rather than storing a second-accurate value.
static void gpsSeedClockFromModuleRtc() {
  ubxSend(0x01, 0x07, nullptr, 0);             // NAV-PVT poll
  uint8_t p[92];
  const int n = ubxAwaitFrame(0x01, 0x07, p, sizeof(p), 800);
  if (n < 24) {
    Serial.println(F("[..] GPS: no NAV-PVT reply - cannot read its RTC"));
    return;
  }

  const uint16_t year = (uint16_t)(p[4] | (p[5] << 8));
  const uint8_t  mon = p[6], day = p[7], hr = p[8], mi = p[9], se = p[10];
  const uint8_t  valid = p[11];                // bit0 date, bit1 time, bit2 resolved
  const uint32_t tAcc = (uint32_t)p[12] | ((uint32_t)p[13] << 8) |
                        ((uint32_t)p[14] << 16) | ((uint32_t)p[15] << 24);
  const uint8_t  fixType = p[20];

  // Report unconditionally: the validity bits are the whole question here, and
  // without them in the log "the clock did not get set" says nothing about why.
  Serial.printf("[..] GPS NAV-PVT %04u-%02u-%02u %02u:%02u:%02u valid=0x%02X"
                " (date=%d time=%d resolved=%d) tAcc=%lu ns fixType=%u\n",
                year, mon, day, hr, mi, se, valid,
                (valid & 0x01) ? 1 : 0, (valid & 0x02) ? 1 : 0,
                (valid & 0x04) ? 1 : 0, (unsigned long)tAcc, fixType);

  if (!(valid & 0x01) || !(valid & 0x02) || year < 2024) {
    // Dump the frame when we could not use it, and only then. "The module does
    // not know the time" and "we are decoding the wrong offsets" look
    // identical from the parsed values alone, and NAV-PVT is 92 bytes on M8/M9
    // - a short reply means it is not the message this code assumes.
    // Reserved bits 4-7 of `valid` reading 1 is itself a sign of misalignment.
    Serial.printf("[..] GPS NAV-PVT unusable, %d-byte payload:", n);
    for (int i = 0; i < n && i < 24; i++) Serial.printf(" %02X", p[i]);
    Serial.println();
    return;
  }

  const long long e = calc_timegm(year, mon, day, hr, mi, se);
  if (e <= CLOCK_SANE_EPOCH) return;

  struct timeval tv = { .tv_sec = (time_t)e, .tv_usec = 0 };
  settimeofday(&tv, nullptr);
  Serial.printf("[OK] clock seeded from the GNSS module's backup RTC"
                " (+/- %lu ms, no fix needed)\n", (unsigned long)(tAcc / 1000000UL));
}

// Key-value config (UBX-CFG-VALSET). Keys verified against u-blox's interface
// description. Layer 0x01 = RAM only: deliberately NOT persisted to the
// module's flash, so a power cycle returns it to defaults and we simply
// reconfigure - which keeps the baud hunt able to find it no matter what.
static void gpsConfigure() {
  if (GPS_TX_GPIO < 0) return;          // no wire to the module's RX

  uint8_t p[96];
  uint16_t n = 0;
  p[n++] = 0x00;                        // version
  p[n++] = 0x01;                        // layers: RAM
  p[n++] = 0x00; p[n++] = 0x00;         // reserved

  auto key = [&](uint32_t k) {
    p[n++] = k & 0xFF; p[n++] = (k >> 8) & 0xFF;
    p[n++] = (k >> 16) & 0xFF; p[n++] = (k >> 24) & 0xFF;
  };
  auto u1 = [&](uint32_t k, uint8_t v)  { key(k); p[n++] = v; };
  auto u2 = [&](uint32_t k, uint16_t v) { key(k); p[n++] = v & 0xFF; p[n++] = v >> 8; };

  u2(0x30210001, GPS_NAV_RATE_MS);      // CFG-RATE-MEAS
  u2(0x30210002, 1);                    // CFG-RATE-NAV (1 solution per measurement)
  // Keep only what we parse. GSV is the fat one (one sentence per 4 satellites,
  // several per epoch); dropping it, GSA, GLL and VTG is what makes 10 Hz fit
  // in 38400 baud. RMC gives time/date/position/speed, GGA altitude/sats/HDOP.
  u1(0x209100bb, 1);                    // GGA on
  u1(0x209100ac, 1);                    // RMC on
  u1(0x209100c5, 0);                    // GSV off
  u1(0x209100c0, 0);                    // GSA off
  u1(0x209100ca, 0);                    // GLL off
  u1(0x209100b1, 0);                    // VTG off

  ubxSend(0x06, 0x8A, p, n);            // CFG-VALSET
  // Compare against ACK_OK explicitly. Assigning the enum straight to the bool
  // made ACK_NAK (2) true, so a module that REJECTED the config - an M8 clone
  // without the M9 keys, say - was logged as configured and then had the baud
  // change sent to it anyway.
  const UbxAck ack = ubxAwaitAck(1200);
  gps.configured = (ack == ACK_OK);
  gps.configAck  = ack == ACK_OK ? 1 : ack == ACK_NAK ? 2 : 3;
  if (!gps.configured) {
    // The two failures point at different faults, which is why ubxAwaitAck
    // distinguishes them: silence means our TX is not reaching the module,
    // a NAK means it heard us and refused the keys.
    if (ack == ACK_NAK) {
      Serial.println(F("[ERR] GPS rejected the config (NAK) - wrong module "
                       "generation? Continuing at its default rate."));
    } else {
      Serial.println(F("[..] GPS config not acknowledged - is GPIO43 wired to "
                       "the module's RX? Continuing at its default rate."));
    }
    return;
  }
  Serial.printf("[OK] GPS configured: %d ms nav rate (%d Hz), GGA+RMC only\n",
                GPS_NAV_RATE_MS, 1000 / GPS_NAV_RATE_MS);

  // Baud change goes in a SEPARATE message, sent last: the ACK for it comes
  // back at the NEW baud, so it cannot be bundled with the config above
  // without losing that ACK. Nothing is persisted to the module's flash, so a
  // power cycle returns it to 38400 and the baud hunt finds it again.
  if (gps.baud != GPS_TARGET_BAUD) {
    uint8_t q[12];
    uint16_t m = 0;
    q[m++] = 0x00; q[m++] = 0x01; q[m++] = 0x00; q[m++] = 0x00;
    const uint32_t k = 0x40520001;      // CFG-UART1-BAUDRATE (U4)
    q[m++] = k & 0xFF; q[m++] = (k >> 8) & 0xFF;
    q[m++] = (k >> 16) & 0xFF; q[m++] = (k >> 24) & 0xFF;
    const uint32_t b = GPS_TARGET_BAUD;
    q[m++] = b & 0xFF; q[m++] = (b >> 8) & 0xFF;
    q[m++] = (b >> 16) & 0xFF; q[m++] = (b >> 24) & 0xFF;
    ubxSend(0x06, 0x8A, q, m);
    delay(100);                         // let it drain before we switch
    gpsSerial.begin(GPS_TARGET_BAUD, SERIAL_8N1, GPS_RX_GPIO, GPS_TX_GPIO);
    gps.baud = GPS_TARGET_BAUD;
    Serial.printf("[OK] GPS link moved to %d baud\n", GPS_TARGET_BAUD);
  }
}

void gps_init() {
  // The queue for the logs, before the task that fills it. Without PSRAM for
  // it, the logs fall back to the latest fix (gps_next_sample).
  ring.slots = (GpsSample *)heap_caps_calloc(GPS_RING_N, sizeof(GpsSample), MALLOC_CAP_SPIRAM);
  if (!ring.slots) Serial.println(F("[ERR] GPS: no PSRAM for the fix queue - a card stall will drop fixes"));
  openPort(0);

#if GPS_SLEEP_ON_PARK
  // Wake a module that we parked in backup mode before the last deep sleep.
  //
  // Any UART activity rouses it, but the first byte is consumed doing the
  // rousing rather than being parsed, so send a short burst of 0xFF: that
  // value can never be mistaken for the start of a UBX frame (0xB5 0x62) or an
  // NMEA sentence ('$'), so a module that is already awake simply discards it.
  // Unconditional on purpose - working out whether we actually parked it would
  // mean trusting RTC state across a wake, and the cost of being wrong is no
  // GPS for the whole drive. Eight bytes at 38400 baud is ~2 ms.
  for (int i = 0; i < GPS_WAKE_PULSE_LEN; i++) gpsSerial.write(0xFF);
  gpsSerial.flush();
  delay(GPS_WAKE_SETTLE_MS);
#endif

  Serial.printf("[..] GPS listening on GPIO%d @ %lu baud\n",
                GPS_RX_GPIO, (unsigned long)gps.baud);

  // Spawn LAST, so everything above - opening the port, the wake burst - has
  // finished touching the UART before the task becomes its owner.
  //
  // 6 KB rather than obd's 4 KB: this one runs Serial.printf with floats in
  // gpsSeedClockFromModuleRtc() and gpsConfigure(), plus UBX frame buffers.
  // A stack overflow here would surface as an intermittent panic with
  // nothing pointing at the GNSS.
  //
  // Priority 4, below CAN's 5: a dropped ISO-TP consecutive frame aborts a
  // whole transfer, whereas a late NMEA sentence costs one fix out of 25.
  xTaskCreatePinnedToCore(gpsTask, "gps", TASK_GPS_STACK, NULL, TASK_GPS_PRIO, NULL, TASK_CORE);
}

#if GPS_SLEEP_ON_PARK
// UBX-RXM-PMREQ (0x02 0x41), version 0, 16-byte form: park the receiver in
// backup mode until something arrives on the UART.
//
// duration = 0 means "indefinitely", which is what we want - the ESP decides
// when to wake, and a duration would just be a second timer to keep in step
// with SLEEP_SAFETY_SEC. flags bit 1 = backup. wakeupSources bit 3 = UARTRX,
// so the wake burst in gps_init() is what brings it back.
//
// There is deliberately no ACK check on the PMREQ itself: it is not
// acknowledged (the receiver is going to sleep, not replying), so waiting for
// one would only add a timeout to every sleep.
//
// Runs ON THE GNSS TASK - see gps_park() below for why.
static void gpsParkNow() {
  const uint8_t pmreq[16] = {
    0x00, 0x00, 0x00, 0x00,        // version 0, reserved
    0x00, 0x00, 0x00, 0x00,        // duration = 0 -> indefinite
    0x02, 0x00, 0x00, 0x00,        // flags: bit1 backup
    0x08, 0x00, 0x00, 0x00,        // wakeupSources: bit3 UARTRX
  };
  ubxSend(0x02, 0x41, pmreq, sizeof(pmreq));
  Serial.println(F("[..] GPS parked (backup mode)"));
}

// Requested from power.cpp immediately before deep sleep, but PERFORMED by the
// GNSS task.
//
// gps_park() is called from goToSleep() on core 1 while the task owns the UART
// on core 0, and HardwareSerial has no locking - two cores writing the same
// port would interleave the PMREQ with whatever the task happened to be
// sending, and a mangled PMREQ leaves the receiver awake. Handing the work to
// the port's single owner removes the race instead of guarding it with a
// mutex, which would otherwise have to be held across the multi-second UBX
// waits above.
void gps_park() {
  parkDone = false;
  parkReq  = true;

  const uint32_t t0 = millis();
  while (!parkDone && !calc_elapsed(millis(), t0, GPS_PARK_ACK_MS)) {
    delay(2);
  }

  // Loud on failure. A park that silently does not happen costs ~17 mA for the
  // entire parked period - the single largest item in the power budget - and
  // absolutely nothing else would ever report it.
  if (!parkDone) {
    Serial.println(F("[ERR] GPS park not acknowledged - receiver may stay awake"));
  }
}
#else
void gps_park() {}
#endif

// One pass: drain the UART, parse, hunt the baud, configure once, keep the
// clock. Called only from gpsTask() below.
static void gpsPoll() {
  // Raw dump of the first bytes we ever see. "Bytes arriving but nothing
  // parses" (wrong baud / noise) and "no bytes at all" (wiring, power, wrong
  // pin) look identical on screen, so tell them apart here.
  static uint16_t dumped = 0;
  static uint32_t lastSentenceMs = 0;          // 0 = none yet
  while (gpsSerial.available()) {
    int c = gpsSerial.read();
    gps.rawBytes++;
    if (dumped < GPS_RAW_DUMP_BYTES) {
      if (dumped == 0) Serial.print(F("[..] GPS raw: "));
      Serial.printf("%02X%c", c, (c >= 32 && c < 127) ? (char)c : '.');
      if (++dumped == GPS_RAW_DUMP_BYTES) Serial.println();
    }
    if (nmea.encode(c)) {
      gps.sentences++;
      lastSentenceMs = millis() ? millis() : 1;
    }
  }

  // The link is sentences arriving NOW, not ever. It used to be
  // "sentences > 0", so a module that died mid-drive never showed FAULT_GPS,
  // and the baud hunt below never resumed for one that came back at another
  // rate (a power-cycled module returns to its 38400 default).
  const uint32_t now = millis();
  const bool wasUp = gps.linkUp;
  gps.linkUp = lastSentenceMs != 0 && now - lastSentenceMs < GPS_LINK_STALE_MS;
  if (wasUp && !gps.linkUp) {
    // Gone: when it is found again, configure it again - after a power cycle
    // it is back to 1 Hz with every sentence on.
    configTried = false;
    gps.configured = false;
    Serial.println(F("[ERR] GPS: no NMEA for a while - link lost, hunting"));
  }

  // No NMEA -> wrong baud (or nothing wired, or the module gone). Try the next.
  if (!gps.linkUp && now - baudTryMs > GPS_BAUD_TRY_MS) {
    baudIdx = (baudIdx + 1) % (sizeof(GPS_BAUDS) / sizeof(GPS_BAUDS[0]));
    openPort(baudIdx);
    Serial.printf("[..] GPS no NMEA (raw bytes seen: %lu), trying %lu baud\n",
                  (unsigned long)gps.rawBytes, (unsigned long)gps.baud);
  }

  // Configure only once we know sentences are decoding, i.e. the baud is
  // right - sending UBX at the wrong baud would just be noise. One-shot, and
  // it blocks ~1 s waiting for the ACK, which is well inside the watchdog.
  if (gps.linkUp && !gps.configured && !configTried) {
    configTried = true;
    gpsConfigure();
    // Straight after config, while we are already blocking on UBX replies and
    // long before a fix is plausible - which is the entire point of it.
    gpsSeedClockFromModuleRtc();
    // Those waits ate the NMEA in between (~2 s): that is not the module
    // going quiet, so do not count it as a lost link.
    lastSentenceMs = millis() ? millis() : 1;
  }

  if (nmea.satellites.isValid()) gps.sats = nmea.satellites.value();
  if (nmea.speed.isValid())      gps.speedKmh = nmea.speed.kmph();
  if (nmea.course.isValid())     gps.courseDeg = nmea.course.deg();
  if (nmea.hdop.isValid())       gps.hdop = nmea.hdop.hdop();

  // A stale fix is worse than no fix - the car keeps moving after signal loss.
  const bool fix = nmea.location.isValid() && nmea.location.age() < GPS_FIX_STALE_MS;
  // Publish only when a NEW position has arrived, not on every 5 ms poll. It
  // used to republish the same fix ~8 times per 40 ms epoch, which churned the
  // seqlock for nothing and made "a new fix" impossible to detect downstream -
  // and lap timing needs exactly that: each fix once, with its own time.
  // isUpdated() is cleared by the lat()/lng() reads below.
  if (fix && nmea.location.isUpdated()) {
    // lround, not a truncating cast: truncation would bias every coordinate
    // toward the equator/meridian by up to 1 cm, consistently in one
    // direction. Exact in double - 1.8e9 is far inside its 53-bit mantissa.
    const int32_t lat = (int32_t)lround(nmea.location.lat() * 1e7);
    const int32_t lon = (int32_t)lround(nmea.location.lng() * 1e7);
    const float   alt = nmea.altitude.isValid() ? (float)nmea.altitude.meters()
                                                : gps.altM;
    // The fix's own UTC time, from the same sentence as the position, in ms
    // since midnight. Lap times are taken from THIS rather than from millis()
    // at parse time, which carries the poll and UART jitter of whenever the
    // sentence happened to be drained.
    const bool     tOk = nmea.time.isValid();
    const uint32_t tMs = tOk ? ((nmea.time.hour() * 60UL + nmea.time.minute()) * 60UL +
                                nmea.time.second()) * 1000UL +
                               nmea.time.centisecond() * 10UL
                             : 0;
    // Publish the position under the seqlock, THEN raise haveFix. The other
    // order let the logger on core 1 see haveFix before any coordinates
    // existed and accept (0,0) as a point - see gps_read_position().
    // Explicit read-then-write rather than ++, which is deprecated on a
    // volatile in C++20. Not a race: this task is the only writer.
    posSeq = posSeq + 1;                          // odd: write in progress
    __sync_synchronize();
    gps.lat1e7 = lat;
    gps.lon1e7 = lon;
    gps.altM   = alt;
    posFixMs   = tMs;
    posTimeOk  = tOk;
    __sync_synchronize();
    posSeq = posSeq + 1;                          // even: consistent
    gps.fixes = gps.fixes + 1;                    // this task is the only writer
    // And into the queue for the logs, every one (gps_next_sample).
    if (ring.slots) {
      GpsSample s;
      sampleNow(&s, GpsFix{lat, lon, alt, tMs, tOk, posSeq});
      fixring_push(&ring, s);
    }
  }
  gps.haveFix = fix;

  // The ESP32 boots with no idea what time it is. NMEA carries UTC, so use it
  // as the clock. Re-synced periodically rather than once, so a long drive
  // can't drift - it costs nothing.
  //
  // Only from a FRESH time. TinyGPSPlus keeps date and time isValid() for
  // ever once seen, so a module that went silent re-stamped the clock with
  // its last, frozen time at every resync - stepping it back by however long
  // the module had been quiet.
  static uint32_t lastSyncMs = 0;
  if (nmea.date.isValid() && nmea.time.isValid() && nmea.date.year() >= 2024 &&
      nmea.time.age() < GPS_TIME_FRESH_MS && nmea.date.age() < GPS_TIME_FRESH_MS &&
      (!gps.timeSet || millis() - lastSyncMs > GPS_TIME_RESYNC_MS)) {
    // calc_timegm(), NOT mktime(). NMEA gives UTC, but mktime() interprets its
    // input as LOCAL time - and settings_apply_tz() has already setenv()'d TZ
    // to the user's zone, so mktime() was subtracting the UTC offset and the
    // system clock ended up 1-2 hours out. Every CSV row and GPX point was
    // stamped with that error while claiming to be UTC. The old comment
    // ("TZ is UTC, so this is UTC") was correct until the timezone setting was
    // added, and then silently stopped being true. timegm() is not declared in
    // this newlib build, so calc.h does the conversion arithmetically - which
    // also makes it testable on the host.
    const long long epoch = calc_timegm(
        nmea.date.year(), nmea.date.month(), nmea.date.day(),
        nmea.time.hour(), nmea.time.minute(), nmea.time.second());
    if (epoch > 0) {
      // To the centisecond the sentence gives, plus how long ago it was
      // parsed - it used to be whole seconds, up to 1 s behind.
      const long long gnssUs = epoch * 1000000LL +
                               nmea.time.centisecond() * 10000LL +
                               (long long)nmea.time.age() * 1000LL;
      struct timeval now;
      gettimeofday(&now, nullptr);
      const long long sysUs = (long long)now.tv_sec * 1000000LL + now.tv_usec;
      switch (calc_clock_fix(sysUs, gnssUs, clock_valid(),
                             GPS_TIME_SLEW_MAX_MS * 1000LL, GPS_TIME_KEEP_MS * 1000LL)) {
        case CALC_CLOCK_STEP: {
          struct timeval tv = { .tv_sec = (time_t)(gnssUs / 1000000LL),
                                .tv_usec = (suseconds_t)(gnssUs % 1000000LL) };
          settimeofday(&tv, nullptr);
          break;
        }
        case CALC_CLOCK_SLEW: {
          const long long d = gnssUs - sysUs;
          struct timeval delta = { .tv_sec = (time_t)(d / 1000000LL),
                                   .tv_usec = (suseconds_t)(d % 1000000LL) };
          adjtime(&delta, nullptr);
          break;
        }
        case CALC_CLOCK_KEEP:
          break;
      }
      if (!gps.timeSet) Serial.println(F("[OK] clock set from GPS (UTC)"));
      gps.timeSet = true;
      lastSyncMs  = millis();
    }
  }
}

// ---- the task ---------------------------------------------------------------
// Core 0, alongside CAN and away from the Arduino loop (and therefore LVGL),
// which CONFIG_ARDUINO_RUNNING_CORE pins to core 1.
//
// The reason it is a task at all: the UBX handshakes block. The CFG-VALSET ACK
// waits up to 1200 ms and the NAV-PVT poll 800 ms (a MON-VER poll, since
// removed, added 1500 ms), and when the module is slow to answer those stack
// up into seconds with nothing rendering -
// which showed as the boot animation freezing mid-sweep. On its own core the
// blocking costs nothing.
//
// Thread safety: every GpsState field is 32 bits or smaller and naturally
// aligned, so each one alone is read and written atomically on Xtensa - the
// same argument obd.h makes for ObdState. That is why position is stored as
// int32 degrees x 1e7 rather than double: an 8-byte double is NOT atomic. But
// one field at a time is not enough for a position, whose lat and lon must
// come from the same fix: those go under the seqlock above, and readers use
// gps_read_position() / gps_read_fix(), never the fields.
#if BENCH_CONSOLE
// Bench 'p': does the module take input at all, and which protocol? The
// carrier's GPS never ACKed its UBX config though its RX pin is driven
// (2026-10-21). u-blox filters input by protocol per port, so NMEA in may
// work with UBX in off: an NMEA poll ($PUBX,00 -> a $PUBX reply) and a UBX
// poll (MON-VER -> B5 62 0A 04) tell "RX dead" from "UBX input off".
// Runs on this task, which owns the UART - like the park.
static volatile bool probeReq = false;
void gps_bench_probe() { probeReq = true; }

static void gpsProbeNow() {
  struct Try { const char *name; const uint8_t *msg; size_t len; const uint8_t *want; size_t wantLen; };
  static const uint8_t pubx[] = "$PUBX,00*33\r\n";
  static const uint8_t pubxWant[] = "$PUBX";
  static const uint8_t monVer[] = { 0xB5, 0x62, 0x0A, 0x04, 0x00, 0x00, 0x0E, 0x34 };
  static const uint8_t monVerWant[] = { 0xB5, 0x62, 0x0A, 0x04 };
  const Try tries[] = {
    { "NMEA $PUBX,00", pubx, sizeof(pubx) - 1, pubxWant, sizeof(pubxWant) - 1 },
    { "UBX MON-VER", monVer, sizeof(monVer), monVerWant, sizeof(monVerWant) },
  };
  Serial.printf("[..] GPS probe at %lu baud\n", (unsigned long)gps.baud);
  for (const Try &t : tries) {
    while (gpsSerial.available()) gpsSerial.read();
    gpsSerial.write(t.msg, t.len);
    gpsSerial.flush();
    size_t matched = 0;
    bool got = false;
    const uint32_t t0 = millis();
    while (!got && millis() - t0 < 2000) {
      if (!gpsSerial.available()) { delay(1); continue; }
      const uint8_t c = gpsSerial.read();
      matched = c == t.want[matched] ? matched + 1 : (c == t.want[0] ? 1 : 0);
      got = matched == t.wantLen;
    }
    Serial.printf("[..] GPS probe %s: %s\n", t.name, got ? "ANSWERED" : "no answer in 2 s");
  }
}
#endif

static void gpsTask(void *arg) {
  (void)arg;
  TaskWdt wdt;
  for (;;) {
    wdt.beat();       // the slowest pass, a UBX handshake, is ~3.5 s
#if BENCH_CONSOLE
    if (probeReq) { probeReq = false; gpsProbeNow(); }
#endif
#if GPS_SLEEP_ON_PARK
    if (parkReq) {
      gpsParkNow();
      parkDone = true;
      // Stop here. The board is about to deep-sleep, and continuing to poll a
      // receiver we just put into backup mode would wake it again on the very
      // next byte we sent it - undoing the 17 mA the park exists to save.
      wdt.stop();
      vTaskDelete(NULL);
    }
#endif
    gpsPoll();
    vTaskDelay(pdMS_TO_TICKS(GPS_POLL_MS));
  }
}

