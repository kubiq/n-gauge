// ============================================================================
// ramlog.h  —  which part holds the internal RAM (2026-11-05).
//
// The heap keeps no owners, so each part is measured as it starts: the free
// internal RAM (and PSRAM) before and after its init. That takes in whatever
// it allocates and the stack of any task it starts. The firmware's own
// static variables come from the linker (_data_start.._bss_end), and what
// was gone before setup() - the core's tasks, USB - from the heap's total.
// Parts that come and go (the WiFi AP) set their figure when they start and
// clear it when they stop. "Other" is the rest: whatever was allocated or
// freed since that no part claimed.
//
// For /api/ram (the app's System card) and the serial console. Written on
// the loop task only (setup, apTask); /api/ram reads it from the web task
// without a lock - the table only grows, and a figure caught mid-update
// is a wrong number on a diagnostics screen, nothing worse.
// ============================================================================
#pragma once
#include <stddef.h>
#include <stdint.h>

#define RAMLOG_MAX 14

// First thing in setup(): the baseline.
void ram_begin();

// The free RAM now, for a part's "before".
struct RamAt { size_t internal, psram; };
RamAt ram_now();

// This part took what went between `before` and now. Called again for the
// same part (the AP, each start), it replaces the figure.
void ram_took(const char *part, const RamAt &before);
// The part let go of it (the AP stopped).
void ram_freed(const char *part);

// {"static":N,"preSetup":N,"parts":[{"part":"drawing","kb":36.4,"psKb":0},
// ...],"otherKb":N} - static and preSetup in bytes, the rest KB with one
// decimal, as JSON, into out. Returns its length, or 0 if out is too small.
int ram_json(char *out, size_t n);

// The same, one line, on the serial console.
void ram_print();
