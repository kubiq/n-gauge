// ============================================================================
// wifitest.cpp  —  see wifitest.h. Empty unless WIFI_GPS_TEST.
// ============================================================================
#include "wifitest.h"

#if WIFI_GPS_TEST
#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <lwip/sockets.h>
#include "config.h"     // AP_MIN_INTERNAL_HEAP, TASK_CORE
#include "logger.h"     // logger_note_boot: the drive's totals, on the card

#if !__has_include("wifitest_secrets.h")
#error "WIFI_GPS_TEST needs wifitest_secrets.h - copy wifitest_secrets.example.h"
#endif
#include "wifitest_secrets.h"

WifiTestState wifitest = {};

static bool     active = false;        // this test has the radio
static bool     onHalf = false;
static uint32_t halfStartMs = 0;
static bool     joinedThisHalf = false;
static volatile uint32_t dstAddr = 0;  // the hotspot's address, network order
static volatile bool senderIdle = true;
static TaskHandle_t sender = nullptr;

// Flat out while SENDING: two datagrams a tick, ~22 Mbit/s offered - more
// than the S3 gets on air, so the radio is as busy as it can be. A refusal
// (the TX queue full) waits a tick.
static void senderTask(void *) {
  static uint8_t buf[WIFI_TEST_PKT];
  memset(buf, 0x5A, sizeof(buf));
  int sock = -1;
  for (;;) {
    if (wifitest.phase != WT_SENDING) {
      if (sock >= 0) { close(sock); sock = -1; }
      senderIdle = true;
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    senderIdle = false;
    if (sock < 0) sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
    sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_port = htons(9);            // discard
    to.sin_addr.s_addr = dstAddr;
    for (int i = 0; i < 2; i++) {
      if (sendto(sock, buf, sizeof(buf), 0, (sockaddr *)&to, sizeof(to)) == (int)sizeof(buf))
        wifitest.sentBytes += sizeof(buf);
      else
        wifitest.sendErrs++;
    }
    vTaskDelay(1);
  }
}

static void radioOn() {
  const size_t freeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (freeInternal < AP_MIN_INTERNAL_HEAP) {   // esp_wifi panics when short
    wifitest.refused++;
    Serial.printf("[..] wifitest: %u B internal free - this half stays off\n",
                  (unsigned)freeInternal);
    return;
  }
  if (!sender)
    xTaskCreatePinnedToCore(senderTask, "wtsend", 3072, nullptr, 1, &sender, TASK_CORE);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WT_SSID, WT_PASS);
  wifitest.phase = WT_JOINING;
  joinedThisHalf = false;
  Serial.println(F("[..] wifitest: on - joining the hotspot"));
}

static void radioOff() {
  if (wifitest.phase == WT_OFF) return;
  if (!joinedThisHalf) wifitest.joinFails++;
  wifitest.phase = WT_OFF;
  // The sender sees OFF within a pass of its loop and closes its socket.
  for (int i = 0; i < 10 && !senderIdle; i++) delay(5);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  wifitest.rssi = 0;
  Serial.println(F("[..] wifitest: off"));
}

static void stop() {
  if (!active) return;                 // never touch a radio the AP has
  radioOff();
  active = false;
  char line[120];
  snprintf(line, sizeof(line),
           "wifitest: %u joined, %u never joined, %u refused, %lu KB sent, %lu send errors",
           wifitest.joins, wifitest.joinFails, wifitest.refused,
           (unsigned long)(wifitest.sentBytes / 1024), (unsigned long)wifitest.sendErrs);
  Serial.println(line);
  logger_note_boot(line);
}

void wifitest_task(uint32_t now, bool driving) {
  if (!driving) { stop(); return; }
  if (!active) {                       // a drive starts with an off half
    active = true;
    onHalf = false;
    halfStartMs = now;
  }
  if (now - halfStartMs >= WIFI_TEST_HALF_MS) {
    halfStartMs = now;
    onHalf = !onHalf;
    if (onHalf) radioOn(); else radioOff();
  }
  if (wifitest.phase == WT_OFF) return;
  const bool joined = WiFi.status() == WL_CONNECTED;
  if (wifitest.phase == WT_JOINING && joined) {
    dstAddr = (uint32_t)WiFi.gatewayIP();
    if (!joinedThisHalf) wifitest.joins++;
    joinedThisHalf = true;
    wifitest.phase = WT_SENDING;
    Serial.println(F("[..] wifitest: joined - sending"));
  } else if (wifitest.phase == WT_SENDING && !joined) {
    wifitest.phase = WT_JOINING;       // auto-reconnect is on
  }
  wifitest.rssi = joined ? (int8_t)WiFi.RSSI() : 0;
}

#endif
