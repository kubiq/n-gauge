// ============================================================================
// obd.cpp  —  TWAI + OBD.  Concrete & correct; only the DIDs/formulas in
//             config.h need filling from the sniffer session.
// ============================================================================
#include "obd.h"
#include "config.h"
#include <Arduino.h>
#include "driver/gpio.h"   // RX idle-level report, Rs pad hold
#include "driver/twai.h"
#include <string.h>

ObdState obd;

static void obdTask(void *arg);   // defined below, spawned from obd_init()

// CAN ID of the module whose reply handleUds() is parsing: only the engine
// ECU (0x7E8) and the TPMS module end the request in flight.
//
// The one-shot OBD probe (supported PIDs per ECU, a DID sweep, a bus census)
// and the link benchmark used to live here too. They answered their questions
// (2026-10-03/04: gateway, no torque PIDs, on-reply polling - DECISIONS.md)
// and were removed 2026-10-09.
static uint32_t udsFrom = 0;

// Byte offsets of each wheel's pressure within the DID 0xC00B payload.
// Only the first TPMS_TRUSTED_WHEELS entries are believed - see config.h.
static const uint16_t tpmsPressureOffset[] = TPMS_PRESSURE_OFFSET;

// One request in flight at a time, the next sent OBD_REQ_GAP_MS after its
// reply (or OBD_REQ_TIMEOUT_MS without one) - see config.h.
static bool     reqInFlight = false;
static uint32_t reqSentMs = 0, reqDoneMs = 0;

void obd_transceiver_standby(bool on) {
#if CAN_RS_CONTROL
  const gpio_num_t rs = (gpio_num_t)CAN_RS_GPIO;

  // Lift the latch FIRST. A hold survives a deep-sleep wake, and a held pad
  // ignores writes - so without this the "back to high-speed" call on the wake
  // path would appear to succeed and change nothing, leaving the driver
  // disabled and every OBD request unanswered.
  gpio_hold_dis(rs);

  // Arduino's OUTPUT is (INPUT|OUTPUT) on ESP32, so the pad stays readable
  // while driven - which is what the hold check in obd_init() relies on.
  pinMode(CAN_RS_GPIO, OUTPUT);
  digitalWrite(CAN_RS_GPIO, on ? HIGH : LOW);

  if (on) {
    // Measured working on this board: full system parked fell to 11 mA against
    // 14.2 mA predicted with the transceiver left in high-speed mode.
    //
    // Do not "improve" this into the rtc_gpio_init()/rtc_gpio_hold_en() form
    // on the theory that an RTC pin needs the RTC mux. That was tried, on
    // exactly that theory, to explain a reading that turned out to be a
    // baseline error (11 mA WITH the GNSS compared against 10 mA WITHOUT it).
    // The pair below is what the 11 mA was measured with.
    gpio_hold_en(rs);
    gpio_deep_sleep_hold_en();
  } else {
    // Global, so this would also release a future GPS load-switch hold. Safe
    // as written because goToSleep() re-arms every hold on the way down, and
    // this path only runs from obd_init() on the way up.
    gpio_deep_sleep_hold_dis();
  }
#else
  (void)on;
#endif
}

void obd_init() {
#if CAN_RS_CONTROL
  // Did the Rs latch actually survive the sleep we just woke from?
  //
  // Read it BEFORE releasing the hold. High means the pad was still driven,
  // i.e. the transceiver spent the sleep in standby. Low means the hold was
  // lost and R1 (10K to GND on the breakout) pulled Rs down, so it ran the
  // whole sleep in high-speed mode at ~3.2 mA - which is exactly the failure
  // the pinMode() version had, and is otherwise only visible as a
  // disappointing ammeter reading with no way to tell why.
  //
  // Meaningless on a cold boot: nothing was held, so expect 0 there.
  if (esp_reset_reason() == ESP_RST_DEEPSLEEP) {
    Serial.printf("[..] CAN Rs after sleep: %d (1 = standby held, 0 = hold lost)\n",
                  gpio_get_level((gpio_num_t)CAN_RS_GPIO));
  }
#endif

  // Undo the standby latched before the last deep sleep, so the driver is
  // enabled and we can actually transmit. Must run before twai_start().
  obd_transceiver_standby(false);

  twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(
      (gpio_num_t)CAN_TX_GPIO, (gpio_num_t)CAN_RX_GPIO, TWAI_MODE_NORMAL);
  // The default rx_queue_len is 5, but one oil-temp reply is 7 frames (First
  // Frame + 6 Consecutive) arriving back-to-back, and lv_timer_handler() can
  // block this loop for tens of ms while rendering. That overflowed the queue,
  // dropped a Consecutive Frame and aborted the reassembly - which presented
  // as oil temp updating only occasionally.
  g.rx_queue_len = 32;
  g.tx_queue_len = 10;
  twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
  twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();  // narrow to 0x7E8 later
  obd.linkUp = (twai_driver_install(&g, &t, &f) == ESP_OK) &&
               (twai_start() == ESP_OK);
  if (!obd.linkUp) return;
  twai_reconfigure_alerts(TWAI_ALERT_RX_QUEUE_FULL | TWAI_ALERT_BUS_OFF |
                          TWAI_ALERT_ERR_PASS | TWAI_ALERT_BUS_RECOVERED,
                          NULL);

  // Report the idle level of the RX line, because it decides whether the
  // CAN-edge wake can arm at all - and that decision was previously only
  // visible in /sleeplog.txt, which cannot be produced while USB is attached
  // (USB inhibits sleep), i.e. never on the bench.
  //
  // A healthy idle bus is recessive, so this should read 1. A 0 means the
  // transceiver is not driving the line - unpowered, miswired, or a floating
  // bus with the terminator removed - and power.cpp will fall back to the
  // timer instead of waking on the first frame.
  gpio_set_pull_mode((gpio_num_t)CAN_RX_GPIO, GPIO_PULLUP_ONLY);
  delayMicroseconds(50);
  Serial.printf("[..] CAN RX idle level %d (1 = recessive, edge wake can arm)\n",
                gpio_get_level((gpio_num_t)CAN_RX_GPIO));

  // Core 0: the Arduino loop (and therefore LVGL) is pinned to core 1 by
  // CONFIG_ARDUINO_RUNNING_CORE, so this keeps CAN off the rendering core.
  xTaskCreatePinnedToCore(obdTask, "obd", 4096, NULL, 5, NULL, 0);
}

// --- request senders --------------------------------------------------------
static void sendMode01(uint8_t pid) {
  twai_message_t m = {};
  m.identifier = 0x7DF; m.data_length_code = 8;
  m.data[0] = 0x02; m.data[1] = 0x01; m.data[2] = pid;
  twai_transmit(&m, pdMS_TO_TICKS(20));
}
static void sendMode22(uint16_t reqId, uint16_t did) {
  twai_message_t m = {};
  m.identifier = reqId; m.data_length_code = 8;
  m.data[0] = 0x03; m.data[1] = 0x22; m.data[2] = did >> 8; m.data[3] = did & 0xFF;
  twai_transmit(&m, pdMS_TO_TICKS(20));
}

// Several Mode 01 PIDs in one request to the engine ECU (at most 6).
static void sendMode01Multi(const uint8_t *pids, uint8_t n) {
  twai_message_t m = {};
  m.identifier = 0x7E0; m.data_length_code = 8;
  m.data[0] = 1 + n; m.data[1] = 0x01;
  memcpy(&m.data[2], pids, n);
  twai_transmit(&m, pdMS_TO_TICKS(20));
}

// --- the request schedule ---------------------------------------------------
// What we ask for, and how often. Requests go out one at a time, the next
// OBD_REQ_GAP_MS after the previous reply (obdPoll). Each time, the FIRST
// entry below that is due is sent - so order is priority - and an entry with
// `everyMs` 0 is always due: the filler that takes every slot nothing else
// wants. Everything rides this one queue on purpose: the ISO-TP receiver has
// a single context, so two multi-frame replies overlapping would cost both.
struct ScheduledRequest {
  void   (*send)();
  uint32_t everyMs;      // 0 = whenever nothing else is due
  bool     carOnOnly;    // skip while the car is off
  uint32_t lastMs;       // 0 = never sent, so due at once
};

static void askTyres() { sendMode22(TPMS_REQ_ID, TPMS_MODE22_DID); }
static void askOil() {
#if OIL_USE_MODE22
  sendMode22(0x7E0, OIL_MODE22_DID);
#else
  sendMode01(0x5C);
#endif
}
static void askFast() {
  static const uint8_t pids[] = OBD_MULTI_PIDS;
  static_assert(sizeof(pids) <= 6, "Mode 01 takes at most 6 PIDs per request");
  sendMode01Multi(pids, sizeof(pids));
}
static void askSlow() {
  static const uint8_t pids[] = OBD_SLOW_PIDS;
  static_assert(sizeof(pids) <= 6, "Mode 01 takes at most 6 PIDs per request");
  sendMode01Multi(pids, sizeof(pids));
}
static void askBaro() { sendMode01(0x33); }   // the zero for boost
static void askVolts() {                      // control module voltage, fuel rail
  static const uint8_t pids[] = OBD_VOLTS_PIDS;
  sendMode01Multi(pids, sizeof(pids));
}
static void askHz1() {
  static const uint8_t pids[] = OBD_HZ1_PIDS;
  static_assert(sizeof(pids) <= 6, "Mode 01 takes at most 6 PIDs per request");
  sendMode01Multi(pids, sizeof(pids));
}

static ScheduledRequest schedule[] = {
  //  ask        every             car on only
  { askTyres,  TPMS_POLL_MS,     true  },   // TPMS module: pressures, temps
  { askOil,    OIL_POLL_MS,      false },   // DID 0xE001: oil, dash coolant
  { askBaro,   BARO_POLL_MS,     false },   // PID 0x33: barometric pressure
  { askVolts,  VOLTS_POLL_MS,    false },   // PIDs 0x42, 0x59: battery, fuel rail
  { askHz1,    OBD_HZ1_POLL_MS,  false },   // lambda, outside air, fuel tank, ...
  { askSlow,   OBD_SLOW_POLL_MS, false },   // timing advance, intake air temp
  { askFast,   0,                false },   // 6 PIDs: boost, speed, load, ...
};

// `carOn`: a non-TPMS frame within OBD_STALE_MS. The TPMS module answers with
// the car parked too, and polling it then only keeps it - and the bus - awake.
static void sendNextRequest(bool carOn) {
  const uint32_t now = millis();
  for (ScheduledRequest &r : schedule) {
    if (r.carOnOnly && !carOn) continue;
    if (r.everyMs != 0 && r.lastMs != 0 && now - r.lastMs < r.everyMs) continue;
    r.lastMs = now ? now : 1;
    r.send();
    return;
  }
}

// --- ISO-TP (ISO 15765-2) multi-frame receive -------------------------------
// The oil-temp DID (0xE001) answers with a 45-byte payload split across a
// First Frame + Consecutive Frames, and the ECU only sends the Consecutive
// Frames after we answer the First Frame with a Flow Control. Verified on the
// car: 99.8% and 100% reassembly success across two drives (drive_0015/0016),
// with zero RX overflows.
#define ISOTP_BUF_SIZE   96
#define ISOTP_TIMEOUT_MS 300

static uint8_t  isoBuf[ISOTP_BUF_SIZE];
static uint16_t isoExpected = 0;
static uint16_t isoGot      = 0;
static uint8_t  isoNextSeq  = 1;
static uint32_t isoStartMs  = 0;
static bool     isoActive   = false;
static uint32_t isoId       = 0;   // CAN ID of the responder mid-transfer

static void isoSendFlowControl(uint16_t reqId) {
  twai_message_t m = {};
  m.identifier = reqId;        // back to whichever module is answering
  m.data_length_code = 8;
  m.data[0] = 0x30;            // FlowControl: ContinueToSend
  m.data[1] = 0x00;            // BlockSize 0 = send all remaining
  m.data[2] = 0x00;            // STmin 0 = no separation delay
  twai_transmit(&m, pdMS_TO_TICKS(20));
}

// --- reply parsing ----------------------------------------------------------
// Data bytes of a Mode 01 PID, for walking a multi-PID reply. 0 = not known:
// the walk stops there and hands that PID whatever is left, which is exactly
// right for a single-PID reply.
static uint8_t pidDataLen(uint8_t pid) {
  switch (pid) {
    case 0x00: case 0x20: case 0x40: case 0x60: case 0x80: return 4;   // bitmaps
    case 0x34: return 4;
    case 0x0C: case 0x43: case 0x42: case 0x3C: case 0x3E: case 0x44: case 0x59:
    case 0x5E: case 0x63: return 2;
    case 0x04: case 0x05: case 0x0B: case 0x0D: case 0x0E: case 0x0F: case 0x11:
    case 0x2F: case 0x33: case 0x45: case 0x46: case 0x47: case 0x49: case 0x4A:
    case 0x4C: case 0x5C: return 1;
    default: return 0;
  }
}

// One PID's data from a Mode 01 reply. dl = bytes available for it.
static void handlePid(uint8_t pid, const uint8_t *d, uint16_t dl) {
  if (dl < 1) return;
  if (pid == 0x05) {
    obd.coolantPid05C = d[0] - 40; obd.pid05Ms = millis(); obd.havePid05 = true;
#if !COOLANT_FROM_E001
    obd.coolantC = obd.coolantPid05C; obd.coolantMs = obd.pid05Ms; obd.haveCoolant = true;
#endif
  }
  else if (pid == 0x5C) { obd.oilTempC = OIL_TEMP_FROM_MODE01(d);  obd.haveOil = true;
                          obd.oilMs    = millis(); }
  else if (pid == BOOST_MODE01_PID) {
    obd.boostBar = BOOST_BAR_FROM_KPA(d[0], obd.haveBaro ? obd.baroKpa : AMBIENT_KPA);
    obd.haveBoost = true;
    obd.boostMs  = millis();
  }
  else if (pid == 0x0D) { obd.speedKmh = d[0]; obd.speedMs = millis(); obd.haveSpeed = true; }
  else if (pid == 0x43 && dl >= 2) {
    obd.absLoadPct = (d[0] << 8 | d[1]) * 100.0f / 255.0f;
    obd.absLoadMs = millis(); obd.haveAbsLoad = true;
  }
  // (256A + B) / 1000 V. Under VOLTS_MIN_V is no reading: switching off, the
  // ECU answered 0.00 V for ~11 s before going silent (drive 2026-10-14) -
  // its shutdown, not the battery's - and a logged 0 wrecks any minimum.
  else if (pid == 0x42 && dl >= 2) {
    const float v = (d[0] << 8 | d[1]) / 1000.0f;
    if (v >= VOLTS_MIN_V) { obd.volts = v; obd.voltsMs = millis(); obd.haveVolts = true; }
  }
  // Plausibility guard: a garbage byte must not shift every boost reading.
  else if (pid == 0x33 && d[0] >= 50 && d[0] <= 110) { obd.baroKpa = d[0]; obd.haveBaro = true; }
  else if (pid == 0x49) {
    obd.pedalRaw = d[0]; obd.pedalMs = millis(); obd.havePedal = true;
  }
  else if (pid == 0x0C && dl >= 2) {             // (256A + B) / 4 rpm
    obd.rpm = (d[0] << 8 | d[1]) / 4; obd.rpmMs = millis(); obd.haveRpm = true;
  }
  else if (pid == 0x0E) {                        // A/2 - 64 deg before TDC
    obd.timingDeg = d[0] / 2.0f - 64.0f; obd.timingMs = millis(); obd.haveTiming = true;
  }
  else if (pid == 0x0F) {                        // A - 40 °C
    obd.iatC = d[0] - 40; obd.iatMs = millis(); obd.haveIat = true;
  }
  // For the logs (2026-10-16) - formulas per SAE J1979.
  else if (pid == 0x5E && dl >= 2) {             // (256A + B) / 20 L/h
    obd.fuelLh = (d[0] << 8 | d[1]) / 20.0f; obd.fuelLhMs = millis(); obd.haveFuelLh = true;
  }
  else if (pid == 0x34 && dl >= 2) {             // (256A + B) * 2 / 65536; C, D: current
    obd.lambda = (d[0] << 8 | d[1]) * 2.0f / 65536.0f; obd.lambdaMs = millis(); obd.haveLambda = true;
  }
  else if (pid == 0x44 && dl >= 2) {             // the same scale, commanded
    obd.lambdaCmd = (d[0] << 8 | d[1]) * 2.0f / 65536.0f;
    obd.lambdaCmdMs = millis(); obd.haveLambdaCmd = true;
  }
  else if (pid == 0x46) {                        // A - 40 °C
    obd.ambientC = d[0] - 40; obd.ambientMs = millis(); obd.haveAmbient = true;
  }
  else if (pid == 0x2F) {                        // A * 100 / 255 %
    obd.fuelPct = d[0] * 100.0f / 255.0f; obd.fuelPctMs = millis(); obd.haveFuelPct = true;
  }
  else if (pid == 0x3C && dl >= 2) {             // (256A + B) / 10 - 40 °C, bank 1 sensor 1
    obd.catC = (d[0] << 8 | d[1]) / 10.0f - 40.0f; obd.catMs = millis(); obd.haveCat = true;
  }
  else if (pid == 0x59 && dl >= 2) {             // (256A + B) * 10 kPa, absolute
    obd.railBar = (d[0] << 8 | d[1]) / 10.0f; obd.railMs = millis(); obd.haveRail = true;
  }
}

// Takes a complete UDS payload (single-frame or reassembled): [SID, ...].
static void handleUds(const uint8_t *p, uint16_t len) {
  if (len < 1) return;
  const uint8_t sid = p[0];
  // An answer from either module we ask ends the request in flight - except
  // 7F xx 78, "response pending", which promises the real one later.
  if ((udsFrom == 0x7E8 || udsFrom == TPMS_REQ_ID + 8) &&
      !(sid == 0x7F && len >= 3 && p[2] == 0x78)) {
    reqInFlight = false;
    reqDoneMs = millis();
  }

  if (sid == 0x41 && len >= 3) {
    // Mode 01: [41, PID, data, PID, data, ...] - one PID, or several when we
    // asked for several (OBD_MULTI_PIDS) and the ECU packs them in one reply.
    uint16_t i = 1;
    while (i + 1 < len) {
      const uint8_t pid = p[i];
      const uint8_t n = pidDataLen(pid);
      const uint16_t avail = len - i - 1;
      if (n == 0 || n > avail) { handlePid(pid, &p[i + 1], avail); break; }
      handlePid(pid, &p[i + 1], n);
      i += 1 + n;
    }
  } else if (sid == 0x62 && len >= 3) {    // Mode 22 response: [62,DIDh,DIDl,A,B..]
    const uint16_t did = (p[1] << 8) | p[2];
    const uint8_t *d   = &p[3];
    const uint16_t dl  = len - 3;          // data bytes available after the DID
#if OIL_USE_MODE22
    // Bounds check matters: we index byte 34 of the payload.
    if (did == OIL_MODE22_DID && dl > OIL_MODE22_OFFSET) {
      obd.oilRaw   = d[OIL_MODE22_OFFSET];
      obd.oilTempC = OIL_TEMP_FROM_MODE22(d);
      obd.oilMs    = millis();
      obd.haveOil  = true;
#if COOLANT_FROM_E001
      // Same payload, byte 25: coolant as the dashboard shows it. dl > 34
      // already covers index 25.
      obd.coolantC    = COOLANT_FROM_MODE22(d);
      obd.coolantMs   = obd.oilMs;
      obd.haveCoolant = true;
#endif
      // Keep the whole payload so the correct byte can be identified offline
      // against dashboard readings - see the note in obd.h.
      obd.oilPayloadLen = dl > sizeof(obd.oilPayload)
                        ? sizeof(obd.oilPayload) : (uint8_t)dl;
      memcpy(obd.oilPayload, d, obd.oilPayloadLen);
    }
#endif
    if (did == TPMS_MODE22_DID) {
      obd.tpmsRawLen = dl > sizeof(obd.tpmsRaw) ? sizeof(obd.tpmsRaw) : (uint8_t)dl;
      memcpy(obd.tpmsRaw, d, obd.tpmsRawLen);
      // pressure = raw/5 psi, temp = raw-50 C. raw 0x00/0xFF means the sensor
      // has not reported yet - the sensors are motion-activated and sleep when
      // parked, so this is the normal state until the car has been driven.
      // Only decode slots whose offset is actually confirmed. Showing a
      // healthy tyre as 1.19 bar is worse than showing nothing.
      for (int i = 0; i < TPMS_TRUSTED_WHEELS; i++) {
        const uint16_t pi = tpmsPressureOffset[i], ti = pi + 1;
        if (ti >= dl) break;
        const uint8_t praw = d[pi];
        // A single 0x00/0xFF does NOT mean "no tyre" - the module answers
        // intermittently while the sensors wake up, and blanking the wheel
        // here made pressures appear, vanish and reappear on the car. Leave
        // the last good value standing and let it age out via tyreMs.
        if (praw == 0x00 || praw == 0xFF) continue;
        obd.tyreBar[i]   = TPMS_BAR_FROM_RAW(praw);
        obd.tyreTempC[i] = TPMS_TEMP_FROM_RAW(d[ti]);
        obd.tyreMs[i]    = millis();
        obd.haveTyre[i]  = true;
      }
    }
#if BOOST_USE_MODE22
    if (did == BOOST_MODE22_DID && dl >= 2) {
      obd.boostBar = /*TODO formula*/ ((d[0] << 8 | d[1]) / 1000.0f);
      obd.haveBoost = true;
      obd.boostMs   = millis();
    }
#endif
  }
  // A negative response (PID/DID unsupported): left as no-update.
}

// Replies we care about: the engine ECU range, plus the TPMS module. Left
// deliberately narrow - anything else on the bus is not ours to parse.
static bool isOurResponse(uint32_t id) {
  return (id >= 0x7E8 && id <= 0x7EF) || id == (TPMS_REQ_ID + 8);
}

static void handleFrame(const twai_message_t &m) {
  if (!isOurResponse(m.identifier)) return;

  // Never read past the frame's own DLC. This core's TWAI HAL copies only dlc
  // bytes into m.data and does NOT zero the rest, and m is reused for every
  // iteration of the receive loop - so data[dlc..7] hold the tail of the
  // PREVIOUS frame. A short final Consecutive Frame (some ECUs don't pad it)
  // would otherwise splice bytes from an unrelated frame into the payload.
  const uint8_t dlc = m.data_length_code > 8 ? 8 : m.data_length_code;
  if (dlc < 2) return;                           // no PCI + data to read
  const uint8_t pci = m.data[0] & 0xF0;

  if (pci == 0x00) {                             // Single Frame
    uint8_t len = m.data[0] & 0x0F;
    if (len > dlc - 1) len = dlc - 1;
    udsFrom = m.identifier;
    handleUds(&m.data[1], len);
  } else if (pci == 0x10) {                      // First Frame
    // Two modules answer us - the engine ECU on 0x7E8 and TPMS on 0x7A8 - and
    // both reply multi-frame. With one shared context and no responder check,
    // a TPMS First Frame could reset the buffer mid-oil-transfer, and the
    // oil module's Consecutive Frames would then be appended to the TPMS
    // payload because only the sequence number was validated. That completed
    // "successfully": engine bytes decoded as tyre pressures, which lands
    // around 1.96 bar and trips a false low-pressure warning for the next two
    // minutes. Reject the interloper and count it, rather than silently
    // corrupting both transfers.
    if (isoActive && m.identifier != isoId) { obd.isoAborts++; return; }
    isoId       = m.identifier;
    isoExpected = ((m.data[0] & 0x0F) << 8) | m.data[1];
    if (isoExpected > ISOTP_BUF_SIZE) isoExpected = ISOTP_BUF_SIZE;
    isoGot = 0;
    for (int i = 2; i < dlc && isoGot < isoExpected; i++) isoBuf[isoGot++] = m.data[i];
    isoNextSeq = 1;
    isoActive  = true;
    isoStartMs = millis();
    isoSendFlowControl(m.identifier - 8);   // request ID of this responder
  } else if (pci == 0x20 && isoActive) {         // Consecutive Frame
    // Must come from the module whose transfer is actually in flight.
    if (m.identifier != isoId) { obd.isoAborts++; return; }
    if ((m.data[0] & 0x0F) != (isoNextSeq & 0x0F)) {
      isoActive = false; obd.isoAborts++; return;
    }
    for (int i = 1; i < dlc && isoGot < isoExpected; i++) isoBuf[isoGot++] = m.data[i];
    isoNextSeq++;
    if (isoGot >= isoExpected) {
      isoActive = false;
      obd.isoOk++;
      udsFrom = isoId;
      handleUds(isoBuf, isoGot);
    }
  }
}

// Runs on its own task - see the note in obd.h.
static void obdPoll() {
  // Block briefly on the queue rather than spinning: this is a dedicated task,
  // so waiting here is free and reacts the instant a frame lands.
  // Zero-initialised every iteration: the HAL fills only data_length_code
  // bytes and leaves the rest as whatever the previous frame left behind.
  twai_message_t m = {};
  while (twai_receive(&m, pdMS_TO_TICKS(5)) == ESP_OK) {
    // "Bus alive" means the CAR is awake: any frame EXCEPT the TPMS module's
    // replies. Those are answers to our own polling, and the module answers
    // with the car off - on 2026-10-02 it did so every 5 s for 5 hours
    // parked, so the bus never looked idle, the pod never slept, and it kept
    // logging and polling (drive_2026-10-02_12-11.csv: no engine replies for
    // those 5 h, 121 TPMS transfers per 10 min). The engine ECU is silent
    // with the car off, so its replies - and any broadcast traffic - are the
    // signal. This also stops a parked wake from opening a drive log.
    if (m.identifier != TPMS_REQ_ID + 8) obd.lastFrameMs = millis();
    handleFrame(m);
  }

  uint32_t now = millis();
  if (isoActive && now - isoStartMs > ISOTP_TIMEOUT_MS) {
    isoActive = false;
    obd.isoAborts++;
  }

  // --- controller health ----------------------------------------------------
  // The TWAI peripheral counts transmit errors and at 256 enters BUS-OFF, where
  // it stops transmitting AND receiving permanently until recovery is asked for
  // explicitly. Without this the gauge would just freeze on stale values after
  // a bad connector or a bus that slept while we stayed powered - with no clue
  // on screen as to why. Seen on the bench: txOk stuck at 6 forever.
  uint32_t alerts = 0;
  if (twai_read_alerts(&alerts, 0) == ESP_OK) {
    if (alerts & TWAI_ALERT_RX_QUEUE_FULL) obd.rxOverflows++;
    if (alerts & TWAI_ALERT_ERR_PASS)      obd.errPassive++;

    if (alerts & TWAI_ALERT_BUS_OFF) {
      obd.busOffs++;
      obd.busOff = true;
      twai_initiate_recovery();       // takes 128 idle bus occurrences
    }
    if (alerts & TWAI_ALERT_BUS_RECOVERED) {
      // Recovery leaves the driver STOPPED; it must be started again.
      if (twai_start() == ESP_OK) {
        obd.busOff = false;
        obd.recoveries++;
      }
    }
  }

  // Nothing else is worth attempting while the controller is down.
  if (obd.busOff) return;

  const uint32_t lastCar = obd.lastFrameMs;
  const bool carOn = lastCar != 0 && (int32_t)(now - lastCar) < (int32_t)OBD_STALE_MS;

  // Pace our own requests: one in flight, the next OBD_REQ_GAP_MS after its
  // reply, or after OBD_REQ_TIMEOUT_MS without one - and never mid-transfer.
  if (reqInFlight && now - reqSentMs >= OBD_REQ_TIMEOUT_MS) {
    reqInFlight = false;
    reqDoneMs = now;
  }
  if (!reqInFlight && !isoActive && now - reqDoneMs >= OBD_REQ_GAP_MS) {
    reqInFlight = true;
    reqSentMs = now;
    sendNextRequest(carOn);                  // what goes out: see `schedule`
  }
}

static void obdTask(void *arg) {
  (void)arg;
  for (;;) {
    obdPoll();
    // obdPoll() already blocks up to 5 ms in twai_receive when the bus is
    // quiet, so this only yields when frames are arriving back-to-back.
    vTaskDelay(1);
  }
}
