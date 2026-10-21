// ============================================================================
// gps.h  —  NMEA in, UBX out, u-blox NEO-M9N (GY-GPSV3 breakout).
//   Four wires: module VCC -> 3V3, GND -> GND, module TX -> GPS_RX_GPIO
//   (header "RX"), module RX -> GPS_TX_GPIO (header "TX").
//   We do transmit: UBX-CFG-VALSET to raise the nav rate at init, and
//   UBX-RXM-PMREQ to park the module before sleep. See config.h for why the
//   two directions are not interchangeable on these particular pins.
// ============================================================================
#pragma once
#include <stdint.h>
#include <stdbool.h>

struct GpsState {
  // linkUp distinguishes "module not wired / wrong baud" from "wired but no
  // sky view yet" - without it the two look identical on screen.
  bool     linkUp    = false;   // valid NMEA sentences are arriving
  bool     haveFix   = false;   // position solution is valid and fresh
  uint8_t  sats      = 0;
  float    speedKmh  = 0.0f;
  float    courseDeg = 0.0f;    // track over ground, degrees from north (RMC)
  float    hdop      = 0.0f;
  // Position as degrees x 1e7, which is u-blox's own native format - the
  // module transmits it this way and TinyGPSPlus widens it to double on the
  // way out, so storing int32 converts it back rather than losing anything.
  //
  // int32 spans +/-214.7 deg, covering both ranges, at 1e-7 deg ~= 1.1 cm. A
  // double over +/-180 deg resolves to sub-nanometre, i.e. eight orders of
  // magnitude finer than the receiver is accurate even with RTK.
  //
  // The width is the point, not the precision: a 32-bit aligned load/store is
  // atomic on Xtensa, an 8-byte double is not. With doubles, moving the GNSS
  // onto core 0 (as CAN already is) would let a reader on core 1 catch a
  // half-updated coordinate and log a position that never existed - the same
  // class of bad point the GPX jump filter exists to reject, but generated
  // internally and impossible to trace.
  int32_t  lat1e7    = 0;
  int32_t  lon1e7    = 0;
  float    altM      = 0.0f;    // altitude, metres - GPX tracks want it
  uint32_t sentences = 0;       // total sentences decoded OK
  uint32_t rawBytes  = 0;       // bytes seen on the wire, parsed or not
  uint32_t baud      = 0;       // rate we ended up talking at
  bool     timeSet   = false;   // system clock has been set from NMEA UTC
  bool     configured = false;  // module accepted our rate/message config
  // How the last configuration attempt went: 0 not tried, 1 ACK, 2 NAK (it
  // heard and refused), 3 no answer (our TX not reaching its RX). For the
  // boot log - a module left at its 1 Hz default looks fine on screen.
  uint8_t  configAck = 0;
  uint32_t fixes     = 0;       // positions published since boot - their rate
};

extern GpsState gps;

// A consistent snapshot of the position: lat and lon from the SAME fix, never
// half of one and half of the next. Returns false if no position has been
// published yet. Use this rather than reading gps.lat1e7/lon1e7 directly
// anywhere that needs the pair to agree - they are written on core 0.
bool gps_read_position(int32_t *lat1e7, int32_t *lon1e7, float *altM);

// Everything about the most recent fix, read as one consistent unit - the
// position together with the fix's own time. For lap timing, which needs both
// to belong to the same fix and needs to notice each new fix exactly once.
struct GpsFix {
  int32_t  lat1e7, lon1e7;
  float    altM;
  uint32_t fixMs;    // UTC ms since midnight, from the same NMEA sentence
  bool     timeOk;   // false if the sentence carried no valid time
  uint32_t seq;      // changes with every newly published fix
};
bool gps_read_fix(GpsFix *out);

// There is deliberately no degrees-as-double accessor here. Nothing needs one:
// calc_implied_kmh() takes the 1e7 form and subtracts before scaling, and
// calc_fmt_deg() formats it as exact decimal text. Adding one would put the
// float conversion back at every call site for no gain.

void gps_init();   // open the UART, and wake a parked module
// gps_init() spawns a task on core 0 that owns the UART from then on, so there
// is deliberately no gps_task() to call from loop() any more - the same
// arrangement obd_init() uses, and for the same reason. The UBX handshakes
// block for up to ~3.5 s between them when the module is slow to answer, and
// on the loop's core that froze the renderer mid-animation.
//
// Thread safety: every GpsState field is 32 bits or smaller and naturally
// aligned, so each one alone is read atomically on Xtensa - no lock for a
// single field. Position is int32 degrees x 1e7 to keep that true (an 8-byte
// double is not atomic), but a position is two fields from one fix, so it is
// published under a seqlock: read it with gps_read_position() or
// gps_read_fix(), which never hand back lat from one fix and lon from the next.

// Put the module into backup mode before deep sleep (UBX-RXM-PMREQ), so it
// stops computing 25 fixes a second for a parked car. Measured at 17.2 mA of a
// 24 mA parked draw - the single largest consumer in the system. A no-op when
// GPS_SLEEP_ON_PARK is 0. gps_init() sends the wake burst that undoes it.
void gps_park();

// Bench builds only (BENCH_CONSOLE, the 'p' command): ask the module for an
// NMEA and a UBX reply and print which came back - see gps.cpp.
void gps_bench_probe();
