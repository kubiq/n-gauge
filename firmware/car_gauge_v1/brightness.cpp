// ============================================================================
// brightness.cpp  —  display brightness by day and night. See brightness.h.
// ============================================================================
#include "brightness.h"
#include "config.h"
#include "calc.h"       // calc_sun_elevation_deg, host-tested
#include "settings.h"
#include "gps.h"
#include "rtc.h"        // clock_valid()
#include "bsp.h"
#include <Arduino.h>
#include <time.h>

// Where the sun is computed for: the last GPS fix, kept through deep sleep so
// a parked wake at night does not light up at full brightness before the
// module reacquires. Before any fix at all, the middle of Czechia.
RTC_DATA_ATTR static float nightLat = NIGHT_LAT_DEFAULT;
RTC_DATA_ATTR static float nightLon = NIGHT_LON_DEFAULT;

static bool isNight = false;

bool brightness_is_night() { return isNight; }

void brightness_update(bool force) {
  static uint32_t lastCheckMs = 0;
  static int applied = -1;
  const uint32_t now = millis();
  if (!force && lastCheckMs != 0 && now - lastCheckMs < NIGHT_CHECK_MS) return;
  lastCheckMs = now ? now : 1;

  int32_t lat1e7, lon1e7;
  if (gps.haveFix && gps_read_position(&lat1e7, &lon1e7, nullptr)) {
    nightLat = lat1e7 / 1e7f;
    nightLon = lon1e7 / 1e7f;
  }
  // No clock, no idea whether it is dark: stay at the day value rather than
  // guess - a dim screen in daylight is the worse failure.
  if (!settings.nightOn || !clock_valid()) {
    isNight = false;
  } else {
    // Hysteresis: night below NIGHT_SUN_ELEV_DEG, day again only above it +
    // NIGHT_HYST_DEG. The sun itself crosses once, but a GPS position update
    // or a clock correction near the threshold could otherwise toggle the
    // screen; 1 degree is ~6 minutes of the sun's travel at dusk.
    const double elev =
        calc_sun_elevation_deg((double)time(nullptr), nightLat, nightLon);
    if (isNight) isNight = elev < NIGHT_SUN_ELEV_DEG + NIGHT_HYST_DEG;
    else         isNight = elev < NIGHT_SUN_ELEV_DEG;
  }

  const int target = isNight ? settings.nightBright : settings.brightness;
  if (force || target != applied) {
    applied = target;
    bsp_display_set_brightness((uint8_t)target);
  }
}
