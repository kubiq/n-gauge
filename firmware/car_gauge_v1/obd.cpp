// ============================================================================
// obd.cpp  —  TWAI + OBD.  Concrete & correct; only the DIDs/formulas in
//             config.h need filling from the sniffer session.
// ============================================================================
#include "obd.h"
#include "config.h"
#include "calc.h"         // ISO-TP reassembly, the Mode 01 walk and formulas
#include "taskwdt.h"
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
static void askOil() { sendMode22(0x7E0, OIL_MODE22_DID); }   // oil and dash coolant
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
// The reassembly is calc_isotp_rx() in calc.h, host-tested; this sends the
// Flow Control it asks for and counts the outcomes.
#define ISOTP_TIMEOUT_MS 300

static CalcIsoTp iso;

static void isoSendFlowControl(uint16_t reqId) {
  twai_message_t m = {};
  m.identifier = reqId;        // back to whichever module is answering
  m.data_length_code = 8;
  m.data[0] = 0x30;            // FlowControl: ContinueToSend
  m.data[1] = 0x00;            // BlockSize 0 = send all remaining
  m.data[2] = 0x00;            // STmin 0 = no separation delay
  twai_transmit(&m, pdMS_TO_TICKS(20));
}

// --- the raw payloads -------------------------------------------------------
// See obd_copy_raw() in obd.h. A spinlock, held for a 48-byte memcpy at most.
static portMUX_TYPE rawMux = portMUX_INITIALIZER_UNLOCKED;

uint8_t obd_copy_raw(ObdRaw which, uint8_t *out, uint8_t outMax) {
  const uint8_t *src = which == OBD_RAW_OIL ? obd.oilPayload : obd.tpmsRaw;
  portENTER_CRITICAL(&rawMux);
  uint8_t n = which == OBD_RAW_OIL ? obd.oilPayloadLen : obd.tpmsRawLen;
  if (n > outMax) n = outMax;
  memcpy(out, src, n);
  portEXIT_CRITICAL(&rawMux);
  return n;
}

// --- reply parsing ----------------------------------------------------------
// One PID's data from a Mode 01 reply. dl = bytes available for it. The
// formulas are calc_mode01_value() in calc.h (SAE J1979, host-tested); what
// is kept, and the guards on it, are here.
static void handlePid(uint8_t pid, const uint8_t *d, uint16_t dl) {
  float v;
  if (!calc_mode01_value(pid, d, dl, &v)) return;
  const uint32_t now = millis();
  switch (pid) {
    // Manifold pressure, kPa absolute: boost is it against the barometer.
    case BOOST_MODE01_PID:
      obd.boostBar = BOOST_BAR_FROM_KPA((uint8_t)v, obd.haveBaro ? obd.baroKpa : AMBIENT_KPA);
      obd.boostMs = now; obd.haveBoost = true;
      break;
    case 0x05:
      obd.coolantPid05C = (int)v; obd.pid05Ms = now; obd.havePid05 = true;
#if !COOLANT_FROM_E001
      obd.coolantC = obd.coolantPid05C; obd.coolantMs = now; obd.haveCoolant = true;
#endif
      break;
    case 0x0D: obd.speedKmh = (int)v;    obd.speedMs = now;   obd.haveSpeed = true;   break;
    case 0x43: obd.absLoadPct = v;       obd.absLoadMs = now; obd.haveAbsLoad = true; break;
    // Under VOLTS_MIN_V is no reading: switching off, the ECU answered 0.00 V
    // for ~11 s before going silent (drive 2026-10-14) - its shutdown, not the
    // battery's - and a logged 0 wrecks any minimum.
    case 0x42:
      if (v >= VOLTS_MIN_V) { obd.volts = v; obd.voltsMs = now; obd.haveVolts = true; }
      break;
    // Plausibility guard: a garbage byte must not shift every boost reading.
    case 0x33:
      if (v >= 50 && v <= 110) { obd.baroKpa = (uint8_t)v; obd.haveBaro = true; }
      break;
    case 0x49: obd.pedalRaw = (uint8_t)v; obd.pedalMs = now;  obd.havePedal = true;  break;
    case 0x0C: obd.rpm = (int)v;          obd.rpmMs = now;    obd.haveRpm = true;    break;
    case 0x0E: obd.timingDeg = v;         obd.timingMs = now; obd.haveTiming = true; break;
    case 0x0F: obd.iatC = (int)v;         obd.iatMs = now;    obd.haveIat = true;    break;
    // For the logs (2026-10-16).
    case 0x5E: obd.fuelLh = v;            obd.fuelLhMs = now; obd.haveFuelLh = true; break;
    case 0x34: obd.lambda = v;            obd.lambdaMs = now; obd.haveLambda = true; break;
    case 0x44: obd.lambdaCmd = v;      obd.lambdaCmdMs = now; obd.haveLambdaCmd = true; break;
    case 0x46: obd.ambientC = (int)v;    obd.ambientMs = now; obd.haveAmbient = true; break;
    case 0x2F: obd.fuelPct = v;          obd.fuelPctMs = now; obd.haveFuelPct = true; break;
    case 0x3C: obd.catC = v;                obd.catMs = now;  obd.haveCat = true;     break;
    case 0x59: obd.railBar = v;            obd.railMs = now;  obd.haveRail = true;    break;
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
    // Mode 01: one PID, or several when we asked for several (OBD_MULTI_PIDS)
    // and the ECU packs them in one reply.
    calc_mode01_walk(p, len, handlePid);
  } else if (sid == 0x62 && len >= 3) {    // Mode 22 response: [62,DIDh,DIDl,A,B..]
    const uint16_t did = (p[1] << 8) | p[2];
    const uint8_t *d   = &p[3];
    const uint16_t dl  = len - 3;          // data bytes available after the DID
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
      portENTER_CRITICAL(&rawMux);
      obd.oilPayloadLen = dl > sizeof(obd.oilPayload)
                        ? sizeof(obd.oilPayload) : (uint8_t)dl;
      memcpy(obd.oilPayload, d, obd.oilPayloadLen);
      portEXIT_CRITICAL(&rawMux);
    }
    if (did == TPMS_MODE22_DID) {
      portENTER_CRITICAL(&rawMux);
      obd.tpmsRawLen = dl > sizeof(obd.tpmsRaw) ? sizeof(obd.tpmsRaw) : (uint8_t)dl;
      memcpy(obd.tpmsRaw, d, obd.tpmsRawLen);
      portEXIT_CRITICAL(&rawMux);
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
  const uint8_t *payload;
  uint16_t len;
  switch (calc_isotp_rx(&iso, m.identifier, m.data, m.data_length_code, millis(),
                        &payload, &len)) {
    case CALC_ISO_SINGLE:
      udsFrom = m.identifier;
      handleUds(payload, len);
      break;
    case CALC_ISO_FIRST:
      isoSendFlowControl(m.identifier - 8);   // request ID of this responder
      break;
    case CALC_ISO_DONE:
      obd.isoOk++;
      udsFrom = m.identifier;
      handleUds(payload, len);
      break;
    case CALC_ISO_ABORT:
      obd.isoAborts++;                         // counted, never corrupting a transfer
      break;
    case CALC_ISO_NONE:
      break;
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
  if (calc_isotp_expire(&iso, now, ISOTP_TIMEOUT_MS)) obd.isoAborts++;

  // --- controller health ----------------------------------------------------
  // The TWAI peripheral counts transmit errors and at 256 enters BUS-OFF, where
  // it stops transmitting AND receiving permanently until recovery is asked for
  // explicitly. Without this the gauge would just freeze on stale values after
  // a bad connector or a bus that slept while we stayed powered - with no clue
  // on screen as to why. Seen on the bench: txOk stuck at 6 forever.
  static bool needStart = false;          // recovered, not yet started again
  uint32_t alerts = 0;
  if (twai_read_alerts(&alerts, 0) == ESP_OK) {
    if (alerts & TWAI_ALERT_RX_QUEUE_FULL) obd.rxOverflows++;
    if (alerts & TWAI_ALERT_ERR_PASS)      obd.errPassive++;

    if (alerts & TWAI_ALERT_BUS_OFF) {
      obd.busOffs++;
      obd.busOff = true;
      twai_initiate_recovery();       // takes 128 idle bus occurrences
    }
    // Recovery leaves the driver STOPPED; it must be started again.
    if (alerts & TWAI_ALERT_BUS_RECOVERED) needStart = true;
  }
  // Until it actually starts. A single attempt on the RECOVERED alert left
  // CAN dead for good if twai_start() failed: busOff stayed true, and the
  // alert does not come again - only a reboot brought the gauge back.
  static uint32_t lastStartTryMs = 0;
  if (needStart && (lastStartTryMs == 0 || now - lastStartTryMs >= CAN_START_RETRY_MS)) {
    lastStartTryMs = now ? now : 1;
    if (twai_start() == ESP_OK) {
      needStart = false;
      lastStartTryMs = 0;
      obd.busOff = false;
      obd.recoveries++;
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
  if (!reqInFlight && !iso.active && now - reqDoneMs >= OBD_REQ_GAP_MS) {
    reqInFlight = true;
    reqSentMs = now;
    sendNextRequest(carOn);                  // what goes out: see `schedule`
  }
}

static void obdTask(void *arg) {
  (void)arg;
  TaskWdt wdt;
  for (;;) {
    wdt.beat();
    obdPoll();
    // obdPoll() already blocks up to 5 ms in twai_receive when the bus is
    // quiet, so this only yields when frames are arriving back-to-back.
    vTaskDelay(1);
  }
}
