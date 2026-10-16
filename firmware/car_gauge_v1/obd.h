// ============================================================================
// obd.h  —  CAN (TWAI) + OBD request/parse.  This layer is hardware-generic.
// ============================================================================
#pragma once
#include <stdint.h>
#include <stdbool.h>

// Live values, updated as replies arrive. NaN/­sentinel until first valid read.
struct ObdState {
  bool     linkUp        = false;   // TWAI driver started
  bool     haveOil       = false;
  bool     haveBoost     = false;
  bool     haveCoolant   = false;
  int      oilTempC      = 0;
  uint8_t  oilRaw        = 0;       // raw DID byte, for offline formula checks
  // The WHOLE DID 0xE001 payload, not just the byte we currently decode.
  // On the first drive our "oil temp" read ~70 C while the dashboard showed
  // ~50 C, and the byte was originally picked for correlating tightly with
  // the coolant curve - which is exactly what a coolant-derived value would
  // do, and what real oil temp would NOT do during warm-up. Keeping the full
  // payload lets the right byte be found offline from one logged drive
  // instead of guessing and reflashing. Same lesson as the TPMS layout.
  uint8_t  oilPayload[48] = { 0 };
  uint8_t  oilPayloadLen  = 0;
  uint32_t oilMs         = 0;       // when oilTempC was last actually updated,
                                    // so a stale value can be shown as "--"
  int      coolantC      = 0;       // as the dashboard shows it - see COOLANT_FROM_E001
  int      coolantPid05C = 0;       // raw sensor, Mode 01 PID 0x05 (logged only)
  bool     havePid05     = false;
  uint32_t pid05Ms       = 0;
  float    boostBar      = 0.0f;
  // Same purpose as oilMs. Boost and coolant had none, so once the link died
  // they kept showing their last value forever - number and arc alike.
  uint32_t coolantMs     = 0;
  uint32_t boostMs       = 0;
  // From the 6-PID request - see OBD_MULTI_PIDS in config.h.
  bool     haveSpeed     = false;
  int      speedKmh      = 0;       // PID 0x0D: the gear, and the CSV's obdKmh
  uint32_t speedMs       = 0;
  bool     haveAbsLoad   = false;
  float    absLoadPct    = 0;       // PID 0x43, for a torque estimate
  uint32_t absLoadMs     = 0;
  bool     haveBaro      = false;
  int      baroKpa       = 0;       // PID 0x33, the zero for boost
  bool     haveRpm       = false;
  int      rpm           = 0;       // PID 0x0C, for the track log
  uint32_t rpmMs         = 0;
  bool     haveTiming    = false;
  float    timingDeg     = 0;       // PID 0x0E, deg before TDC (track log)
  uint32_t timingMs      = 0;
  bool     haveIat       = false;
  int      iatC          = 0;       // PID 0x0F, intake air (track log)
  uint32_t iatMs         = 0;
  bool     haveVolts     = false;
  float    volts         = 0;       // PID 0x42, the ECU's supply (VOLTS_POLL_MS)
  uint32_t voltsMs       = 0;
  // For the logs, CSV and track log (2026-10-16) - see OBD_HZ1_PIDS.
  bool     haveFuelLh    = false;
  float    fuelLh        = 0;       // PID 0x5E, engine fuel rate, L/h (fast)
  uint32_t fuelLhMs      = 0;
  bool     haveLambda    = false;
  float    lambda        = 0;       // PID 0x34, measured (wideband O2, B1S1)
  uint32_t lambdaMs      = 0;
  bool     haveLambdaCmd = false;
  float    lambdaCmd     = 0;       // PID 0x44, what the ECU asks for
  uint32_t lambdaCmdMs   = 0;
  bool     haveAmbient   = false;
  int      ambientC      = 0;       // PID 0x46, outside air
  uint32_t ambientMs     = 0;
  bool     haveFuelPct   = false;
  float    fuelPct       = 0;       // PID 0x2F, fuel tank level
  uint32_t fuelPctMs     = 0;
  bool     haveCat       = false;
  float    catC          = 0;       // PID 0x3C, catalyst, bank 1 sensor 1
  uint32_t catMs         = 0;
  bool     haveRail      = false;
  float    railBar       = 0;       // PID 0x59, fuel rail, absolute
  uint32_t railMs        = 0;
  bool     havePedal     = false;
  uint8_t  pedalRaw      = 0;       // PID 0x49, raw: 37 (14.5 %) = foot off
  uint32_t pedalMs       = 0;
  // TPMS. haveTyre[i] false = that sensor hasn't reported (they are
  // motion-activated and sleep while parked), which must not be shown as 0.0.
  bool     haveTyre[4]   = { false, false, false, false };
  float    tyreBar[4]    = { 0, 0, 0, 0 };
  int      tyreTempC[4]  = { 0, 0, 0, 0 };
  // When each wheel last gave a usable reading. The module answers
  // intermittently while the sensors wake, so the consumer ages values out
  // over TPMS_STALE_MS instead of trusting a single empty reply.
  uint32_t tyreMs[4]     = { 0, 0, 0, 0 };
  // Raw DID 0xC00B payload. Logged verbatim because the byte layout is NOT
  // yet known: only offset 4 (front left pressure) is confirmed. Logging
  // derived values at guessed offsets - as an earlier version did - made the
  // log useless for finding the real ones. Never do that for an unverified
  // format: log the bytes.
  uint8_t  tpmsRaw[48]   = { 0 };
  uint8_t  tpmsRawLen    = 0;
  uint32_t lastFrameMs   = 0;       // last time ANY frame was seen (for sleep)
  // Diagnostics: a dropped Consecutive Frame silently aborts an ISO-TP
  // transaction, which looks like "the value just doesn't update much".
  // Counting it makes that visible instead of mysterious.
  uint32_t isoOk         = 0;       // completed multi-frame reassemblies
  uint32_t isoAborts     = 0;       // aborted (bad sequence or timeout)
  uint32_t rxOverflows   = 0;       // TWAI RX queue overran
  // Controller health. bus-off is latching in hardware: without an explicit
  // recovery the peripheral never speaks again, so these say whether that
  // happened and whether we clawed our way back.
  bool     busOff        = false;   // currently in bus-off / recovering
  uint32_t busOffs       = 0;       // times we hit bus-off
  uint32_t recoveries    = 0;       // successful recoveries
  uint32_t errPassive    = 0;       // error-passive warnings (bus degrading)
};

extern ObdState obd;

// obd_init() installs TWAI and spawns a dedicated task on the OTHER core, so
// CAN reception is never starved by lv_timer_handler() rendering the display
// or by an SD write stalling. There is deliberately no obd_task() to call from
// loop() any more - that coupling is what made oil temp update only
// sporadically (a dropped ISO-TP Consecutive Frame aborts the transaction).
//
// Thread safety: every ObdState field is 32-bit or smaller and naturally
// aligned, so loads/stores are atomic on Xtensa - no mutex needed. Readers can
// briefly see one field updated before another (e.g. haveOil set a moment
// before oilTempC), which is harmless for a display.
void obd_init();

// Is this value current? True only if it has arrived at all AND recently
// (OBD_STALE_MS, or TPMS_STALE_MS for the tyres). The raw have* flags only say
// "arrived once, ever", and stay true after the link dies. The display, the
// drive log and the web status all ask these, so they cannot disagree about
// what "no data" means - they used to: the screen went to "--" while the CSV
// kept repeating the last value in every row.
bool obd_oil_fresh();
bool obd_coolant_fresh();
bool obd_boost_fresh();
bool obd_speed_fresh();
bool obd_absload_fresh();
bool obd_volts_fresh();
bool obd_pedal_fresh();
bool obd_rpm_fresh();
bool obd_timing_fresh();
bool obd_iat_fresh();
bool obd_fuel_lh_fresh();
bool obd_lambda_fresh();
bool obd_lambda_cmd_fresh();
bool obd_ambient_fresh();
bool obd_fuel_pct_fresh();
bool obd_cat_fresh();
bool obd_rail_fresh();
bool obd_tyre_fresh(int wheel);

// Put the transceiver into (or out of) low-current standby by driving its Rs
// pin - see CAN_RS_CONTROL in config.h for the mode table and the wiring
// precondition. A no-op when CAN_RS_CONTROL is 0.
//
// Standby keeps the receiver alive, so this is safe to hold through deep
// sleep without losing the CAN-edge wake. Expect to lose the first frame or
// two on the way back to high-speed mode, which costs nothing because the car
// transmits continuously - but do not treat the first frame after a wake as
// data.
//
// Called with true from goToSleep(), after the wake mask has been sampled.
// There is no matching false call on the wake path: obd_init() releases the
// hold and restores high-speed mode, because a deep-sleep wake always comes
// back through setup().
void obd_transceiver_standby(bool on);
