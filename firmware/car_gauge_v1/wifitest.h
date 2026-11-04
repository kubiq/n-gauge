// ============================================================================
// wifitest.h  —  does WiFi traffic hurt the GPS? A test build only:
//   FW_TEST_DEFINE=WIFI_GPS_TEST ./scripts/build.sh gauge flash
//
// Why (2026-11-03): the logs are to go to the home server over the phone's
// hotspot, which the phone turns on in the car - so with the gauge
// transmitting a few centimetres from the GNSS antenna. The AP is down while
// driving (AP_DRIVE_OFF) on the premise that it hurts the fix, never
// measured. An upload is far more airtime than the AP's beacons.
//
// While driving, WIFI_TEST_STEP_MS each, round and round (WT_STEPS in the
// .cpp): the radio off; joined to the hotspot (wifitest_secrets.h, 2.4 GHz)
// and idle at full power; sending UDP flat out at 8.5 dBm; the same at
// 2 dBm. The datagrams go to the hotspot's own address, the discard port:
// the radio as busy as an upload, no mobile data used. The drive CSV gets
// the phase, the TX power, the rate and the RSSI, and gps.cn0Top4 (GSV, on
// in this build only): within one drive, under the same sky.
//
// First drives (2026-11-04, full power, off/sending only): sending cost
// 6.5 dB of C/N0, every time; sats and fix held. Free internal RAM fell to
// 23 KB - flooding UDP fills esp_wifi's dynamic TX buffers.
// ============================================================================
#pragma once
#include <stdint.h>

#define WIFI_TEST_STEP_MS   60000
#define WIFI_TEST_PKT       1400       // bytes per datagram, under the MTU

// The CSV's wtPhase. (IDLE is 3 so the first drives' 0/1/2 kept meaning.)
enum : uint8_t { WT_OFF, WT_JOINING, WT_SENDING, WT_IDLE };

struct WifiTestState {
  volatile uint8_t  phase;
  int8_t            rssi;              // dBm while joined, else 0
  int8_t            txQdbm;            // TX power set, 0.25 dBm; 0 = off
  volatile uint32_t sentBytes;         // since boot
  uint32_t          sendErrs;          // sendto refusals (TX queue full)
  uint16_t          joins;             // radio-on spells that joined
  uint16_t          joinFails;         // that never did
  uint16_t          refused;           // skipped: internal RAM short
};
extern WifiTestState wifitest;

// From loop() (apTask), every pass. `driving`: the AP is down for a drive
// and nothing else has the radio - otherwise the test lets go of it.
void wifitest_task(uint32_t now, bool driving);
