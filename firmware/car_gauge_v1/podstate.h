// ============================================================================
// podstate.h  —  the gauge's one picture of where things stand, and what
//                follows from it. Pure: a function of its arguments, like
//                calc.h, so the host tests and the replay (tests/replay) run
//                the code the board runs.
//
// Asked 2026-10-23. Before it, each part judged the car by itself from raw
// readings, each its own way: sleep by CAN frames, the WiFi by GPS speed over
// 20 km/h for 5 s, the thinner parked logging by GPS speed under 3 km/h for 3
// minutes, sleep's "moving" by GPS speed over 10, the drive log's `car` column
// by CAN and rpm. They could disagree, and each new feature added an opinion.
//
// Three layers, worked out together once per loop() pass (podstate_step):
//
//   facts      what the modules know, copied in (PsFacts). Each module still
//              owns its own; the card's remounts, the GPS's set-up, the lap
//              timer's steps stay inside them.
//   situation  the car (off / on / running) and its motion (still / moving /
//              driving / parked), with every hold-off time in one place.
//   decisions  what the parts act on: the WiFi wanted or not, the log rate,
//              sleep allowed or what holds it off (awakeBecause), the faults.
//
// Moving goes by the car's own speed (OBD) - exact, and 0 standing - with
// the GPS's as the fallback when the car is not talking (towed, the key out,
// a broken CAN wire), at a threshold above its wander standing still.
//
// One writer: loop() (podstate.cpp). Other tasks read a copy.
// ============================================================================
#pragma once
#include <stdint.h>
#include "calc.h"      // CalcDrive, calc_elapsed

enum PsCar : uint8_t {
  PS_CAR_OFF,        // no frame from the car for staleMs
  PS_CAR_ON,         // talking, the engine not running (ignition on)
  PS_CAR_RUNNING,    // rpm at runningRpm or more
};

enum PsMotion : uint8_t {
  PS_STILL,          // not moving, not long enough to be parked
  PS_MOVING,         // moving, not (yet) a drive
  PS_DRIVING,        // a drive: held through a red light, ended by a stop
  PS_PARKED,         // still for parkAfterMs
};

enum PsSpeedFrom : uint8_t { PS_SPEED_NONE, PS_SPEED_OBD, PS_SPEED_GPS };

enum PsGps : uint8_t {
  PS_GPS_NONE,       // no NMEA at all: no module, wiring, baud
  PS_GPS_SEARCHING,  // talking, no fix
  PS_GPS_FIX,
};

enum PsCard : uint8_t {
  PS_CARD_NONE,      // no card mounted
  PS_CARD_OK,
  PS_CARD_FAILED,    // mounted, but a log could not be written
  PS_CARD_SHARED,    // lent to a PC as a USB drive
};

enum PsWake : uint8_t { PS_WAKE_COLD, PS_WAKE_CAN, PS_WAKE_MOTION, PS_WAKE_BUTTON, PS_WAKE_TIMER };

enum PsFw : uint8_t { PS_FW_OK, PS_FW_PROBATION, PS_FW_ROLLED_BACK };

enum PsLogRate : uint8_t {
  PS_LOG_FULL,       // every row, every fix
  PS_LOG_THIN,       // parked: a row and a point every few seconds
};

// What holds sleep off - all of them clear, it may sleep. Logged, so "why
// did it not sleep" is one column.
enum : uint8_t {
  PS_AWAKE_WAKING = 1u << 0,   // just woken, still finding out why
  PS_AWAKE_USB    = 1u << 1,   // a USB host: bench work
  PS_AWAKE_PHONE  = 1u << 2,   // a phone on the WiFi, or one just left
  PS_AWAKE_USER   = 1u << 3,   // a touch or the button, lately
  PS_AWAKE_MOVING = 1u << 4,   // moving, lately - whatever the bus says
  PS_AWAKE_CAR    = 1u << 5,   // the car talking, lately
  PS_AWAKE_BOOT   = 1u << 6,   // a cold boot that has heard no car yet
};

// The faults the gauge shows - the same bits as ui.h's UiFault (podstate.cpp
// checks), worked out here from the facts.
enum : uint8_t {
  PS_FAULT_OIL_HOT  = 1u << 0,
  PS_FAULT_COOL_HOT = 1u << 1,
  PS_FAULT_CAN      = 1u << 2,   // the driver down, or no frame since boot (past canGraceMs)
  PS_FAULT_GPS      = 1u << 3,   // no NMEA (past gpsGraceMs)
  PS_FAULT_SD       = 1u << 4,   // no card, and not lent to a PC
  PS_FAULT_SD_WRITE = 1u << 5,
};

// The facts, sampled once per pass.
struct PsFacts {
  uint32_t now;                 // millis()
  // The car (obd.h). lastFrameMs counts every frame but the TPMS module's,
  // which answers with the car parked too; 0 = none since boot.
  bool     canLinkUp;
  uint32_t lastFrameMs;
  bool     rpmFresh;
  int16_t  rpm;
  bool     obdKmhFresh;
  int16_t  obdKmh;
  // The GPS (gps.h).
  bool     gpsLink;
  bool     gpsFix;
  float    gpsKmh;
  uint8_t  sats;
  // The card and the clock.
  PsCard   card;
  bool     clockValid;
  // The WiFi.
  bool     apUp;
  uint8_t  apClients;
  bool     downloading;
  // Power.
  bool     usbHost;
  uint32_t lastUserMs;          // a touch or the button; 0 = none since boot
  bool     waking;              // power.cpp's wake check is still listening
  PsWake   wake;
  PsFw     fw;
  // The temperatures past their warnings - the UI's latched states, with
  // their hysteresis.
  bool     oilHot;
  bool     coolHot;
};

// The thresholds: config.h's in the firmware (podstate.cpp), shorter ones in
// the tests.
struct PsCfg {
  uint32_t staleMs;             // no frame this long: the car is off
  int16_t  runningRpm;          // the engine is running from here
  float    obdMovingKmh;        // moving, by the car's speed (0 standing)
  float    gpsMovingKmh;        // moving, by the GPS's - above its wander
  float    driveOnKmh;          // a drive: this fast ...
  uint32_t driveOnMs;           // ... this long
  float    driveOffKmh;         // the drive over: under this ...
  uint32_t driveOffMs;          // ... this long (long: a red light is no end)
  uint32_t parkAfterMs;         // still this long: parked
  uint32_t sleepAfterMs;        // each hold on sleep lasts this long after it ends
  uint32_t coldBootAwakeMs;     // a cold boot with no car stays up this long
  uint32_t canGraceMs;          // no frame yet is not a fault this early
  uint32_t gpsGraceMs;          // no NMEA yet is not a fault this early
};

struct PodState {
  PsFacts     f;                // the facts it was worked out from
  // The situation.
  PsCar       car;
  uint32_t    carSince;
  PsMotion    motion;
  uint32_t    motionSince;
  PsSpeedFrom speedFrom;
  float       kmh;              // from speedFrom; 0 with none
  // The decisions.
  bool        wifiWanted;
  PsLogRate   logRate;
  bool        sleepOk;
  uint8_t     awakeBecause;     // PS_AWAKE_*
  uint8_t     faults;           // PS_FAULT_*
  // Memory between passes.
  CalcDrive   drive;
  uint32_t    stillSince;       // 0 = moving
  uint32_t    lastMovingMs;     // 0 = not since boot
  uint32_t    lastPhoneMs;      // 0 = not since boot
  bool        started;          // a first pass has run
};

// "Since" stamps use 0 as "never"; a pass at millis() 0 takes 1.
static inline uint32_t ps_stamp(uint32_t now) { return now ? now : 1; }

// Within `ms` of the stamp `since` (0 = never: no).
static inline bool ps_within(uint32_t now, uint32_t since, uint32_t ms) {
  return since != 0 && !calc_elapsed(now, since, ms);
}

static inline void podstate_step(PodState *s, const PsFacts &f, const PsCfg &c) {
  const uint32_t now = f.now;
  s->f = f;

  // ---- the car ----
  const bool talking = f.lastFrameMs != 0 && !calc_elapsed(now, f.lastFrameMs, c.staleMs);
  const PsCar car = !talking ? PS_CAR_OFF
                  : f.rpmFresh && f.rpm >= c.runningRpm ? PS_CAR_RUNNING
                  : PS_CAR_ON;
  if (!s->started || car != s->car) { s->car = car; s->carSince = now; }

  // ---- the speed: the car's, else the GPS's ----
  if (f.obdKmhFresh) { s->speedFrom = PS_SPEED_OBD; s->kmh = f.obdKmh; }
  else if (f.gpsFix) { s->speedFrom = PS_SPEED_GPS; s->kmh = f.gpsKmh; }
  else               { s->speedFrom = PS_SPEED_NONE; s->kmh = 0; }
  const bool moving = s->speedFrom == PS_SPEED_OBD ? s->kmh >= c.obdMovingKmh
                    : s->speedFrom == PS_SPEED_GPS ? s->kmh >= c.gpsMovingKmh
                    : false;
  if (moving) { s->lastMovingMs = ps_stamp(now); s->stillSince = 0; }
  else if (!s->stillSince) s->stillSince = ps_stamp(now);

  // ---- the motion ----
  // A drive by calc_drive_update's hysteresis; no speed at all counts towards
  // its end, so one that ends in an underground garage ends.
  const bool driving = calc_drive_update(&s->drive, s->speedFrom != PS_SPEED_NONE, s->kmh, now,
                                         c.driveOnKmh, c.driveOnMs, c.driveOffKmh, c.driveOffMs);
  const PsMotion motion = driving ? PS_DRIVING
                        : moving ? PS_MOVING
                        : calc_elapsed(now, s->stillSince, c.parkAfterMs) ? PS_PARKED
                        : PS_STILL;
  if (!s->started || motion != s->motion) { s->motion = motion; s->motionSince = now; }

  // ---- the decisions ----
  // The WiFi: down while driving - it costs internal RAM the drive's logging
  // wants, and sits by the GNSS antenna - unless a phone is on it.
  s->wifiWanted = !f.waking && !(motion == PS_DRIVING && f.apClients == 0);
  s->logRate = motion == PS_PARKED ? PS_LOG_THIN : PS_LOG_FULL;

  // Sleep: what holds it off, each for sleepAfterMs after it ends (power.cpp
  // sleeps when nothing does).
  if (f.apClients) s->lastPhoneMs = ps_stamp(now);
  uint8_t awake = 0;
  if (f.waking) awake |= PS_AWAKE_WAKING;
  // Bench work: a sleeping board drops off USB (power.cpp, usbAttached).
  if (f.usbHost) awake |= PS_AWAKE_USB;
  // A phone associated - not the AP being up, which it is whenever awake, so
  // that would be never sleeping. Sleeping under someone on the settings
  // page is rude, and mid-download it cuts the file. Walking away ends it.
  if (ps_within(now, s->lastPhoneMs, c.sleepAfterMs)) awake |= PS_AWAKE_PHONE;
  // A touch or the button: someone at the door reading the tyres, engine off.
  if (ps_within(now, f.lastUserMs, c.sleepAfterMs)) awake |= PS_AWAKE_USER;
  // Moving, whatever the bus says: with a broken CAN wire (2026-10-19) the
  // bus was quiet while driving, and the pod slept at 95 km/h, woke on
  // motion, heard nothing, and slept again, every minute.
  if (ps_within(now, s->lastMovingMs, c.sleepAfterMs)) awake |= PS_AWAKE_MOVING;
  // The car talking. Signed (calc_elapsed): the CAN task can stamp a frame
  // a millisecond after `now` was read, and unsigned that read as ~49 days
  // ago - asleep mid-drive, the log split in two.
  if (f.lastFrameMs != 0) {
    if (!calc_elapsed(now, f.lastFrameMs, c.sleepAfterMs)) awake |= PS_AWAKE_CAR;
  } else if (now <= c.coldBootAwakeMs) {
    // No frame ever - engine off, unplugged, or the CAN driver down: a cold
    // boot stays up a while for the screen, then sleeps. Never sleeping on a
    // driver that failed was a flat battery.
    awake |= PS_AWAKE_BOOT;
  }
  s->awakeBecause = awake;
  s->sleepOk = awake == 0;

  uint8_t faults = 0;
  if (f.oilHot) faults |= PS_FAULT_OIL_HOT;
  if (f.coolHot) faults |= PS_FAULT_COOL_HOT;
  // Times since boot, as millis() counts them: plain comparisons.
  const bool canGrace = now < c.canGraceMs || f.waking;
  if (!f.canLinkUp || (f.lastFrameMs == 0 && !canGrace)) faults |= PS_FAULT_CAN;
  if (!f.gpsLink && now > c.gpsGraceMs) faults |= PS_FAULT_GPS;
  if (f.card == PS_CARD_NONE) faults |= PS_FAULT_SD;
  if (f.card == PS_CARD_FAILED) faults |= PS_FAULT_SD_WRITE;
  s->faults = faults;

  s->started = true;
}

// ---- on the gauge (podstate.cpp) ----------------------------------------------
// Once per loop() pass, early: the facts gathered and the state worked out.
// The temperature warnings are loop()'s latched states.
void podstate_update(uint32_t now, bool oilHot, bool coolHot);
// The state as of this pass - from loop() only.
const PodState &podstate();
// A copy, from any task (the logger, the web server): taken under a
// spinlock, a few microseconds.
PodState podstate_copy();

// Names, for the serial log and the replay.
static inline const char *ps_car_name(PsCar c) {
  return c == PS_CAR_OFF ? "off" : c == PS_CAR_ON ? "on" : "running";
}
static inline const char *ps_motion_name(PsMotion m) {
  return m == PS_STILL ? "still" : m == PS_MOVING ? "moving" : m == PS_DRIVING ? "driving" : "parked";
}
