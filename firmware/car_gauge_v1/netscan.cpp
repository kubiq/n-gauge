// ============================================================================
// netscan.cpp  —  see netscan.h.
// ============================================================================
#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <time.h>
#include "netscan.h"
#include "logger.h"     // logger_note_boot: what it saw, on the card
#include "obd.h"        // lastFrameMs: the car was on this wake
#include "rtc.h"        // clock_valid
#include "settings.h"   // apSsid: not our own network
#include "webapi.h"     // web_stop

// Its own namespace, apart from the settings: a blob, the whole NetList.
static const char *NS = "netscan";
static const char *KEY = "seen";

void netscan_load(NetList *out) {
  memset(out, 0, sizeof(*out));
  Preferences p;
  if (!p.begin(NS, /*readOnly=*/true)) return;      // never written yet
  if (p.getBytesLength(KEY) == sizeof(NetList)) p.getBytes(KEY, out, sizeof(NetList));
  p.end();
  if (out->n > NETLIST_MAX) memset(out, 0, sizeof(*out));
}

static void save(const NetList &l) {
  Preferences p;
  p.begin(NS, /*readOnly=*/false);
  p.putBytes(KEY, &l, sizeof(l));
  p.end();
}

void netscan_parked() {
  if (obd.lastFrameMs == 0) return;           // a parked wake: nothing new around
  netscan_now();
}

void netscan_now() {
  web_stop();                                 // the sleep would next; its RAM is the scan's
  WiFi.mode(WIFI_STA);
  esp_task_wdt_reset();
  const uint32_t t0 = millis();
  // Active, the hidden ones left out, 120 ms a channel: ~1.6 s for all 13.
  const int found = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/false,
                                      /*passive=*/false, /*max_ms_per_chan=*/120);
  esp_task_wdt_reset();
  const uint32_t took = millis() - t0;
  NetList l;
  netscan_load(&l);
  const uint8_t before = l.n;
  const uint32_t now = clock_valid() ? (uint32_t)time(nullptr) : 0;
  for (int i = 0; i < found; i++) {
    const String ssid = WiFi.SSID(i);
    if (ssid == settings.apSsid) continue;    // another gauge, or our own echo
    netlist_note(&l, ssid.c_str(), (int8_t)WiFi.RSSI(i), WiFi.encryptionType(i) == WIFI_AUTH_OPEN, now);
  }
  WiFi.scanDelete();
  WiFi.mode(WIFI_OFF);
  if (found > 0) save(l);
  char line[96];
  snprintf(line, sizeof(line), "scan before sleep: %d networks in %lu ms, %u new, %u listed",
           found, (unsigned long)took, (unsigned)(l.n - before), (unsigned)l.n);
  Serial.printf("[..] %s\n", line);
  logger_note_boot(line);
}
