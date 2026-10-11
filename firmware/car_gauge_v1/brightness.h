// ============================================================================
// brightness.h  —  display brightness by day and night.
// ============================================================================
#pragma once
#include <stdbool.h>

// Applies settings.brightness by day and settings.nightBright at night (if
// settings.nightOn), night being the sun NIGHT_SUN_ELEV_DEG below the horizon
// at the last known position. Re-evaluated every NIGHT_CHECK_MS from loop();
// `force` applies at once - at boot, and after the settings page saves.
void brightness_update(bool force);

// For the web status: is it currently treated as night?
bool brightness_is_night();
