// ============================================================================
// crashrec.h  —  where the last crash was, kept across the reset.
//
// A crash said "reset=crash" in the boot log and nothing more: the reason and
// the backtrace go to the USB console as the board panics, and in the car
// there is none (2026-11-12: two crashes closing the upload's tunnel, and no
// way to tell where). So the core's panic hook copies the reason, the
// address and the first addresses of the backtrace into RTC memory, which
// the reset keeps, and the next boot puts them in its line - to be looked up
// in that build's ELF (addr2line; scripts/build.sh keeps one per commit).
// ============================================================================
#pragma once

// From setup(), first thing: the panic hook.
void crashrec_init();
// After a crash: "LoadProhibited core 0 pc 0x42012345 bt 0x... 0x..." -
// once (taken, then cleared); "" otherwise.
const char *crashrec_last();
