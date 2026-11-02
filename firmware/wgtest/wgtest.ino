// WireGuard feasibility on the gauge's ESP32-S3 (2026-10-31): join an allowed
// network, set the clock, bring up a tunnel to the home VPN, fetch a page from
// the server on its LAN - and say what each step costs in internal RAM, the
// gauge's scarce memory (parked, AP down: ~105 KB free).
//
// Settings: copy secrets.example.h to secrets.h (git ignores it) and fill in.
// Build and flash with the gauge's board settings (scripts/build.sh's FQBN):
//   arduino-cli compile --fqbn "<FQBN>" --library firmware/wgtest/lib/WireGuard-ESP32 firmware/wgtest
//   arduino-cli upload  --fqbn "<FQBN>" -p <port> firmware/wgtest
//
// lib/WireGuard-ESP32: Kenta Ida's library, 0.1.5 (BSD-3-Clause, its
// LICENSE), unmaintained since 2022 and written for Arduino core 2.x. Three
// patches for core 3.3 / ESP-IDF 5, each marked in the source:
//   wireguardif.c  the station's netif through esp_netif (tcpip_adapter is gone)
//   wireguard.h    handshake_destroy() declared (GCC 14: an error, not a warning)
//   WireGuard.cpp  netif changes under the TCP/IP core lock (ESP-IDF 5 checks)
#include <WiFi.h>
#include <HTTPClient.h>
#include <WireGuard-ESP32.h>
#include <esp_heap_caps.h>
#include <time.h>
#include "secrets.h"

static WireGuard wg;

static void ram(const char *step) {
  Serial.printf("[ram] %-22s internal free %6u B, largest %6u B, lowest since boot %6u B\n", step,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);   // the core's log_i (the library's handshakes) here too
  delay(3000);
  Serial.println("\n=== WireGuard test ===");
  ram("boot");

  WiFi.mode(WIFI_STA);
  WiFi.begin(T_WIFI_SSID, T_WIFI_PASS);
  const uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(200);
  if (WiFi.status() != WL_CONNECTED) { Serial.println("[ERR] WiFi: not joined"); ram("wifi failed"); return; }
  Serial.printf("[ok] WiFi joined in %lu ms, IP %s, RSSI %d\n", (unsigned long)(millis() - t0),
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
  ram("wifi joined");

  configTime(0, 0, "pool.ntp.org");
  const uint32_t t1 = millis();
  while (time(nullptr) < 1700000000 && millis() - t1 < 15000) delay(200);
  Serial.printf("[%s] clock %lld\n", time(nullptr) > 1700000000 ? "ok" : "ERR", (long long)time(nullptr));

  const uint32_t t2 = millis();
  if (!wg.begin(IPAddress(T_WG_ADDRESS), T_WG_PRIVATE, T_WG_ENDPOINT, T_WG_PEER_PUB, T_WG_PORT)) {
    Serial.println("[ERR] WireGuard: begin failed");
    ram("wg failed");
    return;
  }
  Serial.printf("[ok] WireGuard up in %lu ms\n", (unsigned long)(millis() - t2));
  ram("wg up");

  // The first packets wait for the handshake (the tunnel's link is down
  // until it completes, and a request fails at once): tries for ~30 s.
  for (int i = 0; i < 15; i++) {
    HTTPClient http;
    http.setTimeout(5000);
    const uint32_t t3 = millis();
    http.begin(T_TEST_URL);
    const int code = http.GET();
    const int len = code > 0 ? http.getString().length() : 0;
    http.end();
    Serial.printf("[%s] GET %s -> %d, %d bytes, %lu ms\n", code > 0 ? "ok" : "..", T_TEST_URL, code, len,
                  (unsigned long)(millis() - t3));
    ram("after GET");
    if (code > 0) break;
    delay(2000);
  }
  Serial.println("=== done ===");
}

void loop() {
  static uint32_t last = 0;
  if (millis() - last > 10000) { last = millis(); ram("idle"); }
}
