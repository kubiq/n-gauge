// ============================================================================
// bootlog.h  —  one line per boot on the SD card (/bootlog.txt): what came up
// and how. See logger_note_boot() in logger.h.
// ============================================================================
#pragma once
#include <stdint.h>

// Every loop pass. Writes the line once, BOOT_LOG_AFTER_MS into a boot that
// stays up - a parked timer wake back asleep in seconds writes nothing (the
// sleep log has it).
void bootlog_task(uint32_t now);
