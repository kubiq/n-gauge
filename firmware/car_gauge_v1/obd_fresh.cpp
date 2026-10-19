// ============================================================================
// obd_fresh.cpp  —  "is this value current?" for every OBD reading - see
// obd.h. Its own file, apart from obd.cpp and the CAN driver, so the
// simulator compiles these very functions instead of keeping a copy.
// ============================================================================
#include <Arduino.h>
#include "obd.h"
#include "config.h"
#include "calc.h"       // calc_fresh

// The CAN task writes value, flag and stamp as separate 32-bit stores, so a
// reader on the other core can catch one without the next. The worst case is
// a first reply seen with its stamp still 0: that reads as stale for one
// pass, which is the safe direction.
static bool fresh(bool have, uint32_t stampMs, uint32_t maxAgeMs) {
  return calc_fresh(have, stampMs, millis(), maxAgeMs);
}
bool obd_oil_fresh()     { return fresh(obd.haveOil,     obd.oilMs,     OBD_STALE_MS); }
bool obd_coolant_fresh() { return fresh(obd.haveCoolant, obd.coolantMs, OBD_STALE_MS); }
bool obd_boost_fresh()   { return fresh(obd.haveBoost,   obd.boostMs,   OBD_STALE_MS); }
bool obd_speed_fresh()   { return fresh(obd.haveSpeed,   obd.speedMs,   OBD_STALE_MS); }
bool obd_absload_fresh() { return fresh(obd.haveAbsLoad, obd.absLoadMs, OBD_STALE_MS); }
bool obd_volts_fresh()   { return fresh(obd.haveVolts,   obd.voltsMs,   OBD_STALE_MS); }
bool obd_pedal_fresh()   { return fresh(obd.havePedal,   obd.pedalMs,   OBD_STALE_MS); }
bool obd_rpm_fresh()     { return fresh(obd.haveRpm,     obd.rpmMs,     OBD_STALE_MS); }
bool obd_timing_fresh()  { return fresh(obd.haveTiming,  obd.timingMs,  OBD_STALE_MS); }
bool obd_iat_fresh()     { return fresh(obd.haveIat,     obd.iatMs,     OBD_STALE_MS); }
bool obd_fuel_lh_fresh() { return fresh(obd.haveFuelLh,  obd.fuelLhMs,  OBD_STALE_MS); }
bool obd_lambda_fresh()  { return fresh(obd.haveLambda,  obd.lambdaMs,  OBD_STALE_MS); }
bool obd_lambda_cmd_fresh() { return fresh(obd.haveLambdaCmd, obd.lambdaCmdMs, OBD_STALE_MS); }
bool obd_ambient_fresh() { return fresh(obd.haveAmbient, obd.ambientMs, OBD_STALE_MS); }
bool obd_fuel_pct_fresh() { return fresh(obd.haveFuelPct, obd.fuelPctMs, OBD_STALE_MS); }
bool obd_cat_fresh()     { return fresh(obd.haveCat,     obd.catMs,     OBD_STALE_MS); }
bool obd_rail_fresh()    { return fresh(obd.haveRail,    obd.railMs,    OBD_STALE_MS); }
bool obd_tyre_fresh(int i) {
  if (i < 0 || i >= 4) return false;
  return fresh(obd.haveTyre[i], obd.tyreMs[i], TPMS_STALE_MS);
}
