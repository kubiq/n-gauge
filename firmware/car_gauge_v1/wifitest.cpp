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

// One step each WIFI_TEST_STEP_MS, round and round from the first.
struct WtStep { bool radio; bool send; wifi_power_t power; };
static const WtStep WT_STEPS[] = {
  {false, false, WIFI_POWER_19_5dBm},  // off: the baseline
  {true,  false, WIFI_POWER_19_5dBm},  // joined, idle: about what the AP costs
  {true,  true,  WIFI_POWER_8_5dBm},   // an upload, at a quarter of the power
  {true,  true,  WIFI_POWER_2dBm},     // and at the least that still joins
};
static const uint8_t WT_NSTEPS = sizeof(WT_STEPS) / sizeof(WT_STEPS[0]);

static bool     active = false;        // this test has the radio
static uint8_t  step = 0;
static uint32_t stepStartMs = 0;
static bool     joinedThisSpell = false;
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
  joinedThisSpell = false;
  Serial.println(F("[..] wifitest: on - joining the hotspot"));
}

// The step's power, and the CSV's record of it - set again once joined, in
// case the connect put its own back.
static void setPower() {
  WiFi.setTxPower(WT_STEPS[step].power);
  wifitest.txQdbm = (int8_t)WiFi.getTxPower();
}

static void radioOff() {
  if (wifitest.phase == WT_OFF) return;
  if (!joinedThisSpell) wifitest.joinFails++;
  wifitest.phase = WT_OFF;
  // The sender sees OFF within a pass of its loop and closes its socket.
  for (int i = 0; i < 10 && !senderIdle; i++) delay(5);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  wifitest.rssi = 0;
  wifitest.txQdbm = 0;
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
  if (!active) {                       // a drive starts with the off step
    active = true;
    step = 0;
    stepStartMs = now;
  }
  if (now - stepStartMs >= WIFI_TEST_STEP_MS) {
    stepStartMs = now;
    step = (step + 1) % WT_NSTEPS;
    if (!WT_STEPS[step].radio) radioOff();
    else if (wifitest.phase == WT_OFF) radioOn();   // stays joined between on-steps
    if (wifitest.phase != WT_OFF) setPower();
    Serial.printf("[..] wifitest: step %u\n", step);
  }
  if (wifitest.phase == WT_OFF) return;
  const bool joined = WiFi.status() == WL_CONNECTED;
  const uint8_t want = !joined ? WT_JOINING : WT_STEPS[step].send ? WT_SENDING : WT_IDLE;
  if (want != wifitest.phase) {
    if (wifitest.phase == WT_JOINING) {          // just joined
      dstAddr = (uint32_t)WiFi.gatewayIP();
      if (!joinedThisSpell) wifitest.joins++;
      joinedThisSpell = true;
      setPower();
      Serial.println(F("[..] wifitest: joined"));
    }
    wifitest.phase = want;             // auto-reconnect is on, if it dropped
  }
  wifitest.rssi = joined ? (int8_t)WiFi.RSSI() : 0;
}

#endif
