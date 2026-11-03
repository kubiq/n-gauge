// ============================================================================
// wifitest.h  —  does WiFi traffic hurt the GPS? A test build only:
//   FW_TEST_DEFINE=WIFI_GPS_TEST ./scripts/build.sh gauge flash
//
// Why (2026-11-03): the logs are to go to the home server over the phone's
// hotspot, which the phone turns on in the car - so during the drive, with
// the gauge transmitting a few centimetres from the GNSS antenna. The AP is
// down while driving (AP_DRIVE_OFF) on the premise that it hurts the fix,
// never measured. An upload is far more airtime than the AP's beacons.
//
// While driving, alternately WIFI_TEST_HALF_MS with the radio off and the
// same joined to the hotspot (wifitest_secrets.h, 2.4 GHz) and sending UDP
// flat out - to the hotspot's own address, the discard port: the radio as
// busy as an upload, no mobile data used. The drive CSV gets the phase,
// the rate and the RSSI, and gps.cn0Top4 (GSV, on in this build only):
// within one drive, under the same sky, on vs off.
// ============================================================================
#pragma once
#include <stdint.h>

#define WIFI_TEST_HALF_MS   60000
#define WIFI_TEST_PKT       1400       // bytes per datagram, under the MTU

enum : uint8_t { WT_OFF, WT_JOINING, WT_SENDING };

struct WifiTestState {
  volatile uint8_t  phase;
  int8_t            rssi;              // dBm while joined, else 0
  volatile uint32_t sentBytes;         // since boot
  uint32_t          sendErrs;          // sendto refusals (TX queue full)
  uint16_t          joins;             // on-halves that joined
  uint16_t          joinFails;         // on-halves that never did
  uint16_t          refused;           // on-halves skipped: internal RAM short
};
extern WifiTestState wifitest;

// From loop() (apTask), every pass. `driving`: the AP is down for a drive
// and nothing else has the radio - otherwise the test lets go of it.
void wifitest_task(uint32_t now, bool driving);
