// ============================================================================
// bootlog.cpp  —  one line per boot on the SD card. See bootlog.h.
//
// The serial console says all of this at boot, but in the car there is no
// console - so a GPS left at its 1 Hz default because its configuration never
// arrived (the carrier PCB, 2026-10-19) showed only as drives with a fraction
// of their distance, and nothing on the card said why.
// ============================================================================
#include "bootlog.h"
#include <Arduino.h>
#include "esp_heap_caps.h"
#include "config.h"
#include "gps.h"
#include "obd.h"
#include "rtc.h"
#include "ota.h"
#include "webapi.h"
#include "logger.h"
#include "power.h"

void bootlog_task(uint32_t now) {
  static bool written = false;
  static uint32_t fixesAt = 0;            // gps.fixes at BOOT_LOG_RATE_FROM_MS
  static bool haveFrom = false;
  if (written || now < BOOT_LOG_RATE_FROM_MS) return;
  if (!haveFrom) { haveFrom = true; fixesAt = gps.fixes; return; }
  if (now < BOOT_LOG_AFTER_MS || power_wake_pending()) return;
  written = true;

  // The fix rate the receiver actually delivers, over the last stretch: 25 Hz
  // when our configuration took, 1 Hz at its default.
  const float hz = (gps.fixes - fixesAt) * 1000.0f / (BOOT_LOG_AFTER_MS - BOOT_LOG_RATE_FROM_MS);
  static const char *const ack[] = { "not-tried", "ack", "NAK", "no-answer" };
  char line[300];
  snprintf(line, sizeof(line),
           "fw=%s reset=%s | gps baud=%lu sentences=%lu config=%s rate=%.1fHz fix=%d sats=%u"
           " | clock %s rtc present=%d valid=%d drift=%lds | can driver=%d frames=%d"
           " | card=%d ram=%uKB min=%uKB ap=%d",
           ota_build(), web_reset_text(),
           (unsigned long)gps.baud, (unsigned long)gps.sentences,
           ack[gps.configAck < 4 ? gps.configAck : 0], hz, gps.haveFix ? 1 : 0, gps.sats,
           clock_valid() ? "valid" : "unset", rtc.present ? 1 : 0, rtc.timeValid ? 1 : 0,
           rtc.driftSec, obd.linkUp ? 1 : 0, obd.lastFrameMs ? 1 : 0,
           logger_card_ok() ? 1 : 0,
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024),
           (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024),
           web_is_up() ? 1 : 0);
  Serial.printf("[..] boot log: %s\n", line);
  logger_note_boot(line);
}
