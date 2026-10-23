// ============================================================================
// power.h  —  Option B: sleep when the CAN bus goes quiet, wake when it's back.
// ============================================================================
#pragma once
#include <stdint.h>
#include "podstate.h"   // PsWake

void power_boot_check();   // right after init: if the bus is dead, sleep again
void power_task();         // in loop: sleep once the bus has been idle too long
// True while a deep-sleep wake is still sampling the bus (up to
// WAKE_SAMPLE_MS from boot) - it may yet go back to sleep, so hold off on
// anything expensive that only an awake car needs, like the WiFi AP.
bool power_wake_pending();

// Note that a person is actively operating the gauge - swiping or tapping the
// panel. Each call
// buys another "Sleep after" window (settings.sleepAfterMin), the same a live CAN bus
// and a connected AP client get.
//
// Needed because the idle countdown ran purely off CAN traffic and the AP: with
// the engine off, or on the bench, the panel went dark mid-use with a finger
// still on it. Someone touching the screen is as good a sign of life as a frame.
//
// Records WHEN, not how much is left, matching lastClientMs and
// obd.lastFrameMs. A remaining-time counter decremented in loop() would tie the
// timeout to frame rate rather than to time - loop() runs at a few hundred Hz
// but unevenly, stretched by LVGL rendering and SD writes. Called by the UI on
// a touch; CAN traffic keeps the pod awake on its own, through
// obd.lastFrameMs. A 32-bit timestamp store is naturally atomic, so this is
// safe to call from any task.
void power_note_activity();

// For the gauge's state (podstate.cpp): a USB host seen (sticky, as sleep
// treats it), the last touch or press (0 = none since boot), the "Sleep
// after" window, and what started this boot.
bool power_usb_host();
uint32_t power_last_activity_ms();
uint32_t power_sleep_after_ms();
PsWake power_wake_cause();
