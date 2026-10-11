// ============================================================================
// power.h  —  Option B: sleep when the CAN bus goes quiet, wake when it's back.
// ============================================================================
#pragma once

void power_boot_check();   // right after init: if the bus is dead, sleep again
void power_task();         // in loop: sleep once the bus has been idle too long
// True while a deep-sleep wake is still sampling the bus (up to
// WAKE_SAMPLE_MS from boot) - it may yet go back to sleep, so hold off on
// anything expensive that only an awake car needs, like the WiFi AP.
bool power_wake_pending();

// Note that a person is actively operating the gauge - cycling screens with the
// button or by tapping the panel, or long-pressing to toggle the AP. Each call
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
// but unevenly, stretched by LVGL rendering and SD writes - and a single shared
// counter would be a cross-core read-modify-write, since the CAN task on the
// other core is one of the things that has to reset it. A 32-bit timestamp
// store is naturally atomic, so this is safe to call from anywhere.
void power_note_activity();
