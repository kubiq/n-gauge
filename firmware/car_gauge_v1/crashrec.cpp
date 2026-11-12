// ============================================================================
// crashrec.cpp  —  see crashrec.h.
// ============================================================================
#include <Arduino.h>
#include <esp_attr.h>
#include <string.h>
#include "crashrec.h"

#define CRASH_MAGIC 0x43525348u             // "CRSH"
#define CRASH_BT    6                       // backtrace addresses kept
RTC_NOINIT_ATTR static uint32_t crashMagic;
RTC_NOINIT_ATTR static struct {
  char reason[32];
  int core;
  uint32_t pc;
  uint32_t bt[CRASH_BT];
  uint8_t btLen;
} crash;
static char lastText[200];

// In the panic, on its way to the reset: copies only, nothing that could
// fault again or wait.
static void onPanic(arduino_panic_info_t *info, void *) {
  const char *r = info->reason ? info->reason : "?";
  size_t i = 0;
  for (; r[i] && i < sizeof(crash.reason) - 1; i++) crash.reason[i] = r[i];
  crash.reason[i] = '\0';
  crash.core = info->core;
  crash.pc = (uint32_t)info->pc;
  crash.btLen = (uint8_t)(info->backtrace_len < CRASH_BT ? info->backtrace_len : CRASH_BT);
  for (uint8_t k = 0; k < crash.btLen; k++) crash.bt[k] = info->backtrace[k];
  crashMagic = CRASH_MAGIC;
}

void crashrec_init() { set_arduino_panic_handler(onPanic, nullptr); }

const char *crashrec_last() {
  if (crashMagic == CRASH_MAGIC) {          // once per crash: taken, then cleared
    crashMagic = 0;
    crash.reason[sizeof(crash.reason) - 1] = '\0';
    int n = snprintf(lastText, sizeof(lastText), "%s core %d pc 0x%08lx bt", crash.reason, crash.core,
                     (unsigned long)crash.pc);
    for (uint8_t k = 0; k < crash.btLen && k < CRASH_BT && n > 0 && n < (int)sizeof(lastText); k++)
      n += snprintf(lastText + n, sizeof(lastText) - n, " 0x%08lx", (unsigned long)crash.bt[k]);
  }
  return lastText;
}
