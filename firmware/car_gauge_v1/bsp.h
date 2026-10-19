// ============================================================================
// bsp.h  —  Board Support: the Waveshare-specific glue.
//   These are the ONLY functions that depend on the exact board hardware.
//   Fill bsp.cpp from Waveshare's ESP32-S3-Touch-AMOLED-1.75 example project
//   once the board is in hand. Everything else in this firmware is portable.
// ============================================================================
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "prof.h"   // FRAME_STATS: the loop's ProfStats

// ---- I2C map (SDA=15, SCL=14) ----------------------------------------------
// Every address that answers the boot scan, from the board schematic. Kept
// here because a scan on its own only proves "something is there", and two of
// these are easy to misidentify from their register contents alone.
//
//   0x18  ES8311      audio codec (speaker out)              - driven
//   0x20  TCA9554PWR  8-bit I/O expander                     - NOT driven
//   0x34  AXP2101     PMU                                    - NOT driven
//   0x40  ES7210      4-channel mic ADC (the AEC front end)   - NOT driven
//   0x51  PCF85063    RTC, battery-backed through the AXP2101 - NOT driven
//   0x5A  CST9217     capacitive touch                       - driven
//   0x6B  QMI8658     6-axis IMU                             - driven
//   0x7E  nothing.    A phantom ACK in the I2C-reserved 0x78-0x7F range; it
//         also appears on unrelated Waveshare ESP32-S3 boards whose only
//         shared parts are different, and the schematic has no ninth I2C
//         chip. Do not go looking for a driver for it.
//
// The TCA9554 is the interesting one for future work - it carries signals that
// are otherwise unreachable:
//   P0-P2 EXIO0-2 (free, on the EX0/EX1/EX2 pads)   P3 RTC_INT
//   P4 SYS_OUT = the PWR side button's state         P5 AXP_IRQ
//   P6 QMI8658 INT1                                  P7 GPS_RST (-G variant)
// Its own INT pin is pulled up but not routed to the ESP32, so anything read
// through it must be POLLED. Note the PWR button is only reachable this way;
// the BOOT button (BTN_NEXT_GPIO, GPIO0) is a plain GPIO and needs no I2C.

void bsp_init();          // AXP2101 PMU, I2C bus, CO5300 AMOLED, touch, backlight on
void bsp_lvgl_init();     // register LVGL display + flush cb + tick + input dev
void bsp_display_off();   // AMOLED + backlight off (for sleep; a wake is a
                          // full boot, so there is no matching "on")
void bsp_display_set_brightness(uint8_t b);   // 0-255, applies immediately
#if FRAME_STATS
// One loop pass (sleep excluded) and the lv_timer_handler() in it, fed by
// loop() with PROF_SCOPE.
extern ProfStat bspLoopStat, bspLvglStat;
// The frame/flush/loop summary, every 2 s; call once per loop pass.
void bsp_frame_stats_print(int view);
#else
static inline void bsp_frame_stats_print(int) {}
#endif

// QMI8658 accel in g. gz is the vertical axis - used as a depth cue on the
// g-meter sphere, and it also shows kerb strikes and suspension loading.
// Returns false if no sample has ever been read; otherwise fills gx/gy/gz and
// returns true. When the sensor has no NEW sample it hands back the last good
// one rather than zeros - a logged zero looks exactly like a real reading.
bool bsp_imu_read(float *gx, float *gy, float *gz);
// Wake-on-motion on QMI8658 INT2 -> GPIO21. Stops normal IMU data until the
// next bsp_init(). Threshold in mg (1-255).
bool bsp_imu_wom_arm(uint8_t thresholdMg);
uint8_t bsp_imu_status1();   // bit 2 = wake-on-motion detected; reading clears

// Lowest-power state for everything on the board, called just before deep
// sleep. Measured need: the board alone "asleep" drew 15 mA at 5 V, against an
// ESP32-S3 deep-sleep current of ~20 uA - deep sleep stops the CPU but leaves
// the IMU sampling, the touch controller scanning, the card mounted and the
// audio amp enabled. Everything is re-initialised on the next boot, since a
// wake is a full cold start.
void bsp_prepare_sleep();

// No audio. The ES8311 codec, ES7210 mic ADC and NS4150B amplifier are on the
// board but there is no speaker fitted, so the over-temp warning is visual
// only (red arc and "TOO HOT"). Removed rather than left dormant: the amp
// draws current whenever its enable pin is high, and this is a permanently
// connected install. Recover from git history if a speaker is ever added.
