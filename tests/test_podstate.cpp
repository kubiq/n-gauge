// ============================================================================
// test_podstate.cpp  —  the gauge's state (podstate.h): the car, its motion,
// and what follows - the WiFi, the log rate, sleep and what holds it off -
// with the firmware's own thresholds.
// ============================================================================
#include "test.h"
#include "../firmware/car_gauge_v1/podstate.h"
#include "../firmware/car_gauge_v1/config.h"

static const PsCfg CFG = {
  OBD_STALE_MS, ENGINE_RUNNING_RPM, MOVING_OBD_KMH, SLEEP_HOLD_MOVING_KMH,
  AP_DRIVE_ON_KMH, AP_DRIVE_ON_MS, AP_DRIVE_OFF_KMH, AP_DRIVE_OFF_MS,
  LOG_IDLE_AFTER_MS, BUS_IDLE_SLEEP_MS, COLD_BOOT_AWAKE_MS,
  CAN_MISSING_GRACE_MS, GPS_MISSING_GRACE_MS,
};

// A gauge with a card, a GPS with a fix, and the car as given.
struct Sim {
  PodState s = {};
  PsFacts f = {};
  Sim() {
    f.canLinkUp = true;
    f.gpsLink = true;
    f.card = PS_CARD_OK;
  }
  // The car at `rpm` and `kmh` (OBD), talking, at time t (ms).
  void car(uint32_t t, int rpm, int kmh) {
    f.now = t;
    f.lastFrameMs = t;
    f.rpmFresh = f.obdKmhFresh = true;
    f.rpm = (int16_t)rpm;
    f.obdKmh = (int16_t)kmh;
    podstate_step(&s, f, CFG);
  }
  // The car silent at time t; the GPS as given.
  void silent(uint32_t t, bool fix = true, float gpsKmh = 0) {
    f.now = t;
    f.rpmFresh = f.obdKmhFresh = false;
    f.gpsFix = fix;
    f.gpsKmh = gpsKmh;
    podstate_step(&s, f, CFG);
  }
  // Each second from t0 to t1 (exclusive), the car as given.
  void drive(uint32_t t0, uint32_t t1, int rpm, int kmh) {
    for (uint32_t t = t0; t < t1; t += 1000) car(t, rpm, kmh);
  }
};

static void the_car_is_off_on_or_running() {
  Sim m;
  m.car(1000, 0, 0);
  CHECK_EQ(m.s.car, PS_CAR_ON);            // ignition on: the ECU answers 0 rpm
  m.car(2000, 780, 0);
  CHECK_EQ(m.s.car, PS_CAR_RUNNING);
  CHECK_EQ(m.s.carSince, 2000);
  // No new frame, and no rpm to go by: still talking - on - until a frame is
  // overdue, then off.
  m.silent(2000 + OBD_STALE_MS - 1);
  CHECK_EQ(m.s.car, PS_CAR_ON);
  m.silent(2000 + OBD_STALE_MS);
  CHECK_EQ(m.s.car, PS_CAR_OFF);
}

static void a_drive_takes_the_wifi_down_and_a_red_light_does_not_end_it() {
  Sim m;
  m.drive(1000, 4000, 800, 10);
  CHECK_EQ(m.s.motion, PS_MOVING);         // moving, not a drive yet
  CHECK(m.s.wifiWanted);
  m.drive(4000, 4000 + AP_DRIVE_ON_MS + 1000, 2000, 50);
  CHECK_EQ(m.s.motion, PS_DRIVING);
  CHECK(!m.s.wifiWanted);
  // A red light: 40 s standing, under AP_DRIVE_OFF_MS.
  const uint32_t t = m.s.f.now + 1000;
  m.drive(t, t + 40000, 800, 0);
  CHECK_EQ(m.s.motion, PS_DRIVING);
  CHECK(!m.s.wifiWanted);
  // A phone on the WiFi keeps it up, driving or not.
  m.f.apClients = 1;
  m.car(t + 40000, 800, 0);
  CHECK(m.s.wifiWanted);
  m.f.apClients = 0;
  // Stopped long enough: over.
  m.drive(t + 41000, t + 41000 + AP_DRIVE_OFF_MS + 1000, 800, 0);
  CHECK(m.s.motion != PS_DRIVING);
  CHECK(m.s.wifiWanted);
}

// The car's speed reads 0 standing; the GPS wanders - 0-1 km/h at a pump,
// 2026-10-22. Neither is movement.
static void standing_is_still_by_either_speed() {
  Sim m;
  m.drive(1000, 10000, 800, 0);
  CHECK_EQ(m.s.motion, PS_STILL);
  CHECK_EQ(m.s.speedFrom, PS_SPEED_OBD);
  m.silent(11000, true, 1.5f);
  CHECK_EQ(m.s.speedFrom, PS_SPEED_GPS);
  CHECK_EQ(m.s.motion, PS_STILL);
  m.silent(12000, false);
  CHECK_EQ(m.s.speedFrom, PS_SPEED_NONE);
  CHECK_EQ(m.s.motion, PS_STILL);
}

// The car silent but the GPS moving: towed, or a broken CAN wire
// (2026-10-19). It is moving, and that holds sleep off.
static void the_gps_says_moving_when_the_car_cannot() {
  Sim m;
  m.silent(1000, true, 30.0f);
  CHECK_EQ(m.s.motion, PS_MOVING);
  CHECK(m.s.awakeBecause & PS_AWAKE_MOVING);
}

static void parked_after_a_while_still_thins_the_log() {
  Sim m;
  m.drive(1000, 60000, 2000, 60);
  CHECK_EQ(m.s.logRate, PS_LOG_FULL);
  const uint32_t t = 61000;
  m.drive(t, t + LOG_IDLE_AFTER_MS - 1000, 800, 0);
  CHECK(m.s.motion != PS_PARKED);
  CHECK_EQ(m.s.logRate, PS_LOG_FULL);
  m.drive(t + LOG_IDLE_AFTER_MS - 1000, t + LOG_IDLE_AFTER_MS + 2000, 800, 0);
  CHECK_EQ(m.s.motion, PS_PARKED);
  CHECK_EQ(m.s.logRate, PS_LOG_THIN);
  // Moving again: full at once.
  m.car(t + LOG_IDLE_AFTER_MS + 3000, 1200, 5);
  CHECK_EQ(m.s.motion, PS_MOVING);
  CHECK_EQ(m.s.logRate, PS_LOG_FULL);
}

static void sleep_waits_for_the_car_the_phone_and_the_user() {
  Sim m;
  m.drive(1000, 5000, 800, 0);
  CHECK(!m.s.sleepOk);
  CHECK_EQ(m.s.awakeBecause, PS_AWAKE_CAR);
  // The car off: still held, for BUS_IDLE_SLEEP_MS after its last frame.
  const uint32_t last = m.s.f.lastFrameMs;
  m.silent(last + BUS_IDLE_SLEEP_MS - 1);
  CHECK_EQ(m.s.awakeBecause, PS_AWAKE_CAR);
  m.silent(last + BUS_IDLE_SLEEP_MS);
  CHECK(m.s.sleepOk);
  CHECK_EQ(m.s.awakeBecause, 0);
  // A phone joins: held while on and for the window after it leaves.
  uint32_t t = last + BUS_IDLE_SLEEP_MS + 1000;
  m.f.apClients = 1;
  m.silent(t);
  CHECK_EQ(m.s.awakeBecause, PS_AWAKE_PHONE);
  m.f.apClients = 0;
  m.silent(t + BUS_IDLE_SLEEP_MS - 1);
  CHECK_EQ(m.s.awakeBecause, PS_AWAKE_PHONE);
  m.silent(t + BUS_IDLE_SLEEP_MS);
  CHECK(m.s.sleepOk);
  // A touch, and USB.
  t += BUS_IDLE_SLEEP_MS + 1000;
  m.f.lastUserMs = t;
  m.f.usbHost = true;
  m.silent(t + 10);
  CHECK_EQ(m.s.awakeBecause, PS_AWAKE_USER | PS_AWAKE_USB);
}

// No frame since boot: a cold boot stays up COLD_BOOT_AWAKE_MS, then may
// sleep; the CAN fault only after its grace, not while a wake is checking.
static void a_cold_boot_with_no_car() {
  Sim m;
  m.silent(1000, false);
  CHECK_EQ(m.s.awakeBecause, PS_AWAKE_BOOT);
  CHECK_EQ(m.s.faults & PS_FAULT_CAN, 0);
  m.silent(CAN_MISSING_GRACE_MS, false);
  CHECK(m.s.faults & PS_FAULT_CAN);
  m.silent(COLD_BOOT_AWAKE_MS, false);
  CHECK_EQ(m.s.awakeBecause, PS_AWAKE_BOOT);
  m.silent(COLD_BOOT_AWAKE_MS + 1, false);
  CHECK(m.s.sleepOk);
  // Woken, still listening: held, no fault, no WiFi.
  Sim w;
  w.f.waking = true;
  w.silent(500, false);
  CHECK_EQ(w.s.awakeBecause, PS_AWAKE_WAKING | PS_AWAKE_BOOT);
  CHECK_EQ(w.s.faults & PS_FAULT_CAN, 0);
  CHECK(!w.s.wifiWanted);
}

static void faults_from_the_facts() {
  Sim m;
  m.car(GPS_MISSING_GRACE_MS + 1, 800, 0);
  CHECK_EQ(m.s.faults, 0);
  m.f.gpsLink = false;
  m.f.card = PS_CARD_SHARED;               // lent to a PC is no fault
  m.f.oilHot = true;
  m.car(GPS_MISSING_GRACE_MS + 2, 800, 0);
  CHECK_EQ(m.s.faults, PS_FAULT_GPS | PS_FAULT_OIL_HOT);
  m.f.card = PS_CARD_NONE;
  m.car(GPS_MISSING_GRACE_MS + 3, 800, 0);
  CHECK(m.s.faults & PS_FAULT_SD);
  m.f.card = PS_CARD_FAILED;
  m.car(GPS_MISSING_GRACE_MS + 4, 800, 0);
  CHECK_EQ(m.s.faults & (PS_FAULT_SD | PS_FAULT_SD_WRITE), PS_FAULT_SD_WRITE);
}

int main() {
  printf("test_podstate\n");
  TEST(the_car_is_off_on_or_running);
  TEST(a_drive_takes_the_wifi_down_and_a_red_light_does_not_end_it);
  TEST(standing_is_still_by_either_speed);
  TEST(the_gps_says_moving_when_the_car_cannot);
  TEST(parked_after_a_while_still_thins_the_log);
  TEST(sleep_waits_for_the_car_the_phone_and_the_user);
  TEST(a_cold_boot_with_no_car);
  TEST(faults_from_the_facts);
  TEST_SUMMARY();
}
