// ============================================================================
// ota.h  —  firmware updates without USB: from the phone (settings page) or
//           from /firmware.bin on the SD card, with automatic rollback.
//
// The flash has two 3 MB app slots (partition scheme app3M_fat9M_16MB). An
// update is written into the slot that is NOT running and only switched to
// once complete and verified, so an interrupted upload changes nothing.
//
// A new firmware then boots on PROBATION (the bootloader's app rollback,
// enabled in the core's build). It confirms itself after OTA_HEALTHY_MS of
// running, or just before deep sleep - a wake is a reboot, and an
// unconfirmed image that reboots is rolled back. If it crashes or the
// watchdog fires before confirming, the bootloader boots the previous
// firmware on its own.
// ============================================================================
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// At boot, once the card is mounted: install /firmware.bin if it is there
// and has not been acted on before - each file once, by the SHA-256 in its
// app header (see ota.cpp). The file is left on the card. Reboots on
// success (does not return).
void ota_check_sd();

// From loop(): confirm a probation boot once it has run OTA_HEALTHY_MS.
void ota_task();

// Confirm now (before deep sleep - see above). Harmless when not on probation.
void ota_confirm();

// Phone upload, chunk by chunk (webapi.cpp's upload handler). begin() with
// the total size if known (0 = unknown); end() returns true on a complete,
// valid image - then the caller replies and calls ota_reboot().
bool ota_begin(size_t total);
bool ota_write(const uint8_t *data, size_t len);
bool ota_end();
void ota_abort();
const char *ota_error();          // last error, for the reply
void ota_reboot();                // closes logs, restarts into the new slot

// For the settings page: build stamp, and whether a rollback happened.
const char *ota_build();
bool ota_rolled_back();
// The running firmware's app hash, first 8 hex digits - matches the "(xxxxxxxx)"
// the SD install logs for a file, so a file can be matched to what runs.
const char *ota_hash();
