// ============================================================================
// taskwdt.h  —  the task watchdog for the background tasks.
//
// The watchdog used to watch loop() alone. The CAN, GNSS and logger tasks on
// core 0 were not watched, so one that hung went unnoticed: frozen readings on
// a gauge that looked alive, and - with the GNSS link judged by that same task
// (A3) - not even the GPS fault. Now each subscribes itself and feeds the
// watchdog once a pass, and a hung one reboots the pod like a hung loop does;
// the reset reason (task WDT) is in the boot log.
//
// The tasks start in setup() before the watchdog is configured (it is armed
// last, so a slow init cannot trip it), so each subscribes on its first pass
// after setup() sets taskwdt_armed.
// ============================================================================
#pragma once
#include "esp_task_wdt.h"

inline volatile bool taskwdt_armed = false;

// For a long walk that may run on a watched task or not (a directory scan):
// feeds the watchdog only if this task is subscribed - esp_task_wdt_reset()
// from any other logs an error.
inline void taskwdt_feed() {
  if (esp_task_wdt_status(NULL) == ESP_OK) esp_task_wdt_reset();
}

// One per task, beat() once per pass of its loop.
struct TaskWdt {
  bool on = false;
  void beat() {
    if (!on && taskwdt_armed) on = esp_task_wdt_add(NULL) == ESP_OK;
    if (on) esp_task_wdt_reset();
  }
  // Before the task ends itself (vTaskDelete), or the watchdog fires on it.
  void stop() {
    if (on) esp_task_wdt_delete(NULL);
    on = false;
  }
};
