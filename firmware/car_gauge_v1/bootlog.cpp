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
#include "taskwdt.h"      // taskwdt_last_culprit(): after a watchdog reset
#include "crashrec.h"     // crashrec_last(): after a crash, where
#include "upload.h"       // upload_last_close_step(): the tunnel close, before it

void bootlog_task(uint32_t now) {
  static bool written = false;
  static uint32_t fixesAt = 0;            // gps.fixes at BOOT_LOG_RATE_FROM_MS
  static bool haveFrom = false;
  static char line[480] = "";             // made once; tried until it goes - room for a crash's
  static uint32_t triedAt = 0;
  if (written || now < BOOT_LOG_RATE_FROM_MS) return;
  if (!haveFrom) { haveFrom = true; fixesAt = gps.fixes; return; }
  if (now < BOOT_LOG_AFTER_MS || power_wake_pending()) return;
  // Not on the card yet (busy - an upload reading it): again a little
  // later, the same line. It used to be dropped, and a reset's reason with
  // it (2026-11-08: the one boot whose reason was wanted had no line).
  if (line[0]) {
    if (now - triedAt < 5000) return;
    triedAt = now;
    written = logger_note_boot(line);
    return;
  }

  // The fix rate the receiver actually delivers, over the last stretch: 25 Hz
  // when our configuration took, 1 Hz at its default.
  const float hz = (gps.fixes - fixesAt) * 1000.0f / (BOOT_LOG_AFTER_MS - BOOT_LOG_RATE_FROM_MS);
  static const char *const ack[] = { "not-tried", "ack", "NAK", "no-answer" };
  snprintf(line, sizeof(line),
           "fw=%s reset=%s | gps baud=%lu sentences=%lu config=%s rate=%.1fHz fix=%d sats=%u"
           " | clock %s rtc present=%d valid=%d drift=%lds lost=%d | can driver=%d frames=%d"
           " | card=%d ram=%uKB min=%uKB ap=%d",
           ota_build(), web_reset_text(),
           (unsigned long)gps.baud, (unsigned long)gps.sentences,
           ack[gps.configAck < 4 ? gps.configAck : 0], hz, gps.haveFix ? 1 : 0, gps.sats,
           clock_valid() ? "valid" : "unset", rtc.present ? 1 : 0, rtc.timeValid ? 1 : 0,
           rtc.driftSec, rtc.lostPower ? 1 : 0, obd.linkUp ? 1 : 0, obd.lastFrameMs ? 1 : 0,
           logger_card_ok() ? 1 : 0,
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024),
           (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024),
           web_is_up() ? 1 : 0);
  // After a watchdog reset, which task it was (taskwdt.cpp).
  const char *wdt = taskwdt_last_culprit();
  if (*wdt) {
    const size_t n = strlen(line);
    snprintf(line + n, sizeof(line) - n, " | wdt:%s", wdt);
  }
  // After a crash, where (crashrec.cpp) - and how far the upload's tunnel
  // close got, if it was in one (upload.cpp): both crashes of 2026-11-12
  // came closing it.
  const char *cr = crashrec_last();
  if (*cr) {
    size_t n = strlen(line);
    snprintf(line + n, sizeof(line) - n, " | crash: %s", cr);
    // The step the last tunnel close reached (6: it finished) - from
    // before this crash's reset, kept in RTC memory by the library.
    if (const uint8_t wgStep = upload_last_close_step()) {
      n = strlen(line);
      snprintf(line + n, sizeof(line) - n, " | wgclose: step %u of 6", wgStep);
    }
  }
  Serial.printf("[..] boot log: %s\n", line);
  triedAt = now;
  written = logger_note_boot(line);
}
