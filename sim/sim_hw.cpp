// The gauge's hardware and services, stood in for on the PC. The interfaces
// are the firmware's own headers; what is behind them here is the window
// (bsp), the replayed drive (obd, gps, the IMU) and harmless no-ops for what
// a desk does not have - sleep, OTA, the RTC chip, the WiFi AP's server.

#include <SDL2/SDL.h>
#include <cmath>

#include "Arduino.h"
#include "bsp.h"
#include "brightness.h"
#include "config.h"
#include "gps.h"
#include "logger.h"
#include "lvgl.h"
#include "obd.h"
#include "ota.h"
#include "power.h"
#include "rtc.h"
#include "usbdisk.h"
#include "webapi.h"

SimSerial Serial;
SimEsp ESP;

// ---- OBD: the state the replay fills (sim_replay.cpp) -----------------------
ObdState obd;
// obd_*_fresh(): the firmware's own, obd_fresh.cpp.
void obd_init() { obd.linkUp = true; }   // the CAN driver "started"
void obd_transceiver_standby(bool) {}

// ---- GPS --------------------------------------------------------------------
GpsState gps;
void gps_init() {}
void gps_park() {}

// ---- The board: the window, the mouse, the replayed IMU ----------------------
extern float simImu[3];
static const int LCD_PX = 466;   // the CO5300 panel, as bsp.cpp's LCD_W / LCD_H
void bsp_init() {}
void bsp_lvgl_init() {
  lv_init();
  lv_tick_set_cb([]() -> uint32_t { return millis(); });
  // A dialog, so tiling window managers (i3) float it at the panel's size
  // instead of stretching it over the screen.
  SDL_SetHint("SDL_X11_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_DIALOG");
  lv_display_t *d = lv_sdl_window_create(LCD_PX, LCD_PX);
  lv_sdl_window_set_title(d, "N Gauge (sim)");
  // And a fixed size, as the panel. LVGL 9.2 has no getter for the window;
  // it is its driver data's first field.
  SDL_SetWindowResizable(*(SDL_Window **)lv_display_get_driver_data(d), SDL_FALSE);
  lv_sdl_mouse_create();
}
void bsp_display_off() {}
void bsp_display_set_brightness(uint8_t) {}
bool bsp_imu_read(float *gx, float *gy, float *gz) {
  *gx = simImu[0]; *gy = simImu[1]; *gz = simImu[2];
  return true;
}
bool bsp_imu_wom_arm(uint8_t) { return true; }
uint8_t bsp_imu_status1() { return 0; }
void bsp_prepare_sleep() {}

// ---- Power: never sleeps on the desk ----------------------------------------
void power_boot_check() {}
void power_task() {}
bool power_wake_pending() { return false; }
void power_note_activity() {}

// ---- The card: logging is not part of the simulation (yet) -------------------
void logger_init() {}
void logger_start() {}
void logger_sd_lock() {}
void logger_sd_unlock() {}
bool logger_card_ok() { return true; }
void logger_dump_tail(size_t) {}
void logger_close() {}
float sim_trip_km();
float logger_trip_km() { return sim_trip_km(); }
void logger_note_g(float, float) {}
void logger_note_raw_g(float, float, float) {}
void logger_note_loop_us(uint32_t) {}
void logger_suspend() {}
void logger_resume() {}
bool logger_card_shared() { return false; }
bool logger_is_open(const char *) { return false; }

// ---- USB: "a PC is connected" is the U key (the bench page) ------------------
bool simUsbHost = false;
static bool shared = false;
void usbdisk_init() {}
void usbdisk_task() { if (!simUsbHost) shared = false; }
bool usbdisk_host_connected() { return simUsbHost; }
bool usbdisk_share() { if (!simUsbHost) return false; shared = true; return true; }
void usbdisk_unshare() { shared = false; }
UsbDiskState usbdisk_state() { return shared ? USBDISK_SHARED : USBDISK_IDLE; }

// ---- OTA, the RTC chip, brightness: nothing to do ----------------------------
void ota_check_sd() {}
void ota_task() {}
void ota_confirm() {}
bool ota_begin(size_t) { return false; }
bool ota_write(const uint8_t *, size_t) { return false; }
bool ota_end() { return false; }
void ota_abort() {}
const char *ota_error() { return "not in the simulation"; }
void ota_reboot() {}
const char *ota_build() { return "sim"; }
bool ota_rolled_back() { return false; }
const char *ota_hash() { return "00000000"; }

RtcState rtc;
void rtc_init() {}
void rtc_task() {}
bool rtc_set_from_epoch(long long) { return true; }
bool clock_valid() { return true; }

void brightness_update(bool) {}
bool brightness_is_night() { return false; }

// ---- The WiFi AP: shown as up, with nobody on it ------------------------------
static bool webUp = false;
void web_start() { webUp = true; }
void web_stop() { webUp = false; }
bool web_is_up() { return webUp; }
uint8_t web_clients() { return 0; }
const char *web_ip() { return webUp ? "4.3.2.1" : ""; }
const char *web_reset_text() { return "power on"; }
void web_sd_space_kb(uint32_t *u, uint32_t *t) { *u = 1024 * 1024; *t = 32 * 1024 * 1024; }
void web_task() {}
