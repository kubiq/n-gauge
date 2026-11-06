// ============================================================================
// taskwdt.cpp  —  which task tripped the watchdog, kept across the reset.
//
// A watchdog reset said "watchdog" in the boot log and nothing more: the
// names of the tasks that stopped feeding it go to the ROM console as the
// board panics, and in the car there is none (2026-11-06: the log upload
// reset the bench board, and only a reset reason was left). So the ISR hook
// the watchdog calls before it panics copies the names into RTC memory,
// which a software reset keeps, and the next boot reports them.
// ============================================================================
#include <Arduino.h>
#include <esp_attr.h>
#include <string.h>
#include "taskwdt.h"

#define WDT_WHO_MAGIC 0x57445457u          // "WDTW"
RTC_NOINIT_ATTR static uint32_t whoMagic;
RTC_NOINIT_ATTR static char who[96];
static char lastWho[96];

static void IRAM_ATTR collect(void *, const char *msg) {
  // " loopTask (CPU 1)" and the like, one call each: appended, cut to fit.
  size_t n = strnlen(who, sizeof(who));
  while (*msg && n + 1 < sizeof(who)) who[n++] = *msg++;
  who[n] = '\0';
}

// The watchdog's hook, in its ISR, just before the panic.
extern "C" void IRAM_ATTR esp_task_wdt_isr_user_handler(void) {
  who[0] = '\0';
  esp_task_wdt_print_triggered_tasks(collect, nullptr, nullptr);
  whoMagic = WDT_WHO_MAGIC;
}

const char *taskwdt_last_culprit() {
  if (whoMagic == WDT_WHO_MAGIC) {         // once per reset: taken, then cleared
    who[sizeof(who) - 1] = '\0';
    strlcpy(lastWho, who, sizeof(lastWho));
    whoMagic = 0;
  }
  return lastWho;
}
