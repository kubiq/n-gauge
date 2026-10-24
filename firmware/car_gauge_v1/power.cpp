// ============================================================================
// power.cpp  —  deep-sleep power management for the OBD-powered install.
//
// The pod lives permanently in the dash on constant 12 V (OBD pin 16), so
// parked current is the whole game: whatever it draws, it draws for every hour
// the car is not driven.
//
// Wake is EDGE-TRIGGERED on the CAN RX line, not a timer. An idle CAN bus sits
// recessive (HIGH) and any traffic pulls it dominant (LOW), so the transceiver
// is the wake source: the board sleeps until the car talks, then wakes in
// microseconds. The previous timer-only design woke every SLEEP_POLL_SEC and
// paid for a FULL COLD BOOT each time - panel, LVGL, touch, IMU, SD
// mount, CAN install, GPS UART - which at a 2 s poll is roughly a 50 % duty
// cycle. That is not sleeping, and it also flashed the screen every 2 s.
//
// A long timer wake runs alongside as a safety net, so a mistake in the edge
// configuration cannot leave the board asleep forever with the car running.
// ============================================================================
#include "power.h"
#include "config.h"
#include "obd.h"
#include "logger.h"
#include "webapi.h"
#include <Arduino.h>
#if !ARDUINO_USB_MODE
#include "USB.h"            // (bool)USB: enumerated by a host - see usbAttached()
#endif
#include "esp_sleep.h"
#include "esp_rtc_time.h"   // esp_rtc_get_time_us(): runs through deep sleep
#include "driver/gpio.h"
#include "driver/rtc_io.h"

#include "bsp.h"          // bsp_prepare_sleep()
#include "gps.h"          // gps_park()
#include "ui.h"           // ui_show_notice(): the sleep reason, on the panel
#include "settings.h"     // settings_log_sleep(): the sleep reason, in NVS
#include "ota.h"          // ota_confirm(): a wake is a reboot
#include "calc.h"         // calc_elapsed(): every idle window

// Survives deep sleep (RTC memory), unlike everything else: each wake is a
// fresh boot, so this is the only way to notice that we keep waking up for
// nothing. See the chatter guard in goToSleep().
RTC_DATA_ATTR static uint32_t fruitlessWakes = 0;
// The same for motion wakes - see MOTION_FRUITLESS_MAX.
RTC_DATA_ATTR static uint32_t motionFruitless = 0;
// For the sleeplog: when we last went to sleep, on the RTC clock, which keeps
// running through deep sleep - so the next sleep line can say how long that
// sleep lasted. An instant false re-wake shows as slept=0s.
// (2026-10-08: this replaces logging the INT2 level armed-for vs at-wake,
// which could never tell a false wake from a real one - on ANY ext0 wake the
// pin is at the armed level.)
RTC_DATA_ATTR static uint64_t sleptAtUs = 0;
static uint32_t lastSleepSec = UINT32_MAX;      // UINT32_MAX = not a sleep wake

// Never sleep while a USB host is attached.
//
// Deep sleep powers down USB-Serial-JTAG, so the serial port disappears from
// the PC a second after boot - which also means esptool can no longer open it,
// and reflashing would need the BOOT+RST button dance every single time.
// isPlugged() is SOF-based, so it detects a real host rather than just 5 V on
// the pin: in the car the pod is fed on VBUS with no data lines, so this reads
// false there and sleep behaves normally. That makes bench safety automatic
// rather than something to remember in config.h.
static bool usbAttached() {
#if SLEEP_BENCH_TEST
  return false;          // bench aid: exercise the real parked path
#else
  // STICKY: once a host has been seen since boot, stay awake for good.
  //
  // isPlugged() is SOF-based - it reports whether the host is actively
  // clocking the bus right now - so it goes false when the port is suspended,
  // which desktop USB autosuspend does on its own. The board then decided the
  // cable had gone away and slept, in the middle of bench work, with the cable
  // still plugged in. Latching removes that whole class of surprise.
  //
  // Safe because the two cases are cleanly separated: in the car there is no
  // host at all (the pod is fed on VBUS with no data lines), so the flag never
  // latches and sleep behaves normally. On the bench it latches once and the
  // board stays up until it is power-cycled - which is exactly what you want
  // while working on it.
  static bool everSeen = false;
  if (everSeen) return true;
#if ARDUINO_USB_MODE
  everSeen = Serial.isPlugged();
#else
  // TinyUSB (needed for the card-as-USB-drive, 2026-10-08): (bool)USB is
  // "enumerated by a host". NOT (bool)Serial, which is only true while a
  // terminal holds the port open (DTR) - the pod would sleep on the bench
  // whenever no serial monitor happened to be running.
  everSeen = (bool)USB;
#endif
  return everSeen;
#endif
}

// How long the car must be quiet (and the screen untouched, and no phone on
// the AP) before sleeping: the phone setting "Sleep after", in minutes.
// SLEEP_MEASURE_MODE keeps its own short bench value.
static uint32_t idleSleepMs() {
#if SLEEP_MEASURE_MODE
  return BUS_IDLE_SLEEP_MS;
#else
  return (uint32_t)settings.sleepAfterMin * 60000UL;
#endif
}

// millis() of the last deliberate user interaction; 0 = none since boot.
// See power_note_activity() in power.h for why this is a timestamp.
static uint32_t lastActivityMs = 0;

void power_note_activity() {
  const uint32_t t = millis();
  lastActivityMs = t ? t : 1;           // 0 is the "never" sentinel
}

static void goToSleep() {
  // Decide between edge wake and timer wake.
  //
  // Two cases must fall back to the timer, or the board would spin: waking
  // instantly, booting, finding no usable bus, and sleeping again as fast as
  // it can - which burns far more current than either sleep mode.
  //   1. The line is already dominant. A healthy idle bus is recessive, so LOW
  //      here means a stuck/shorted bus or a dead transceiver, and an ANY_LOW
  //      trigger would fire the instant we slept.
  //   2. We have woken repeatedly without ever finding traffic worth staying
  //      up for. Some cars keep modules chattering long after key-off; if that
  //      is happening, back off and poll slowly instead.
  // Pull the line up BEFORE reading it, and keep the pull through sleep.
  //
  // The first on-car sleeplog showed canRx flipping between 0 and 1 across
  // sleeps, which sent us to the 300 s timer about half the time - up to five
  // minutes before the gauge wakes after the key turns. The cause: with the
  // car off the bus is idle and the transceiver's receiver output is not
  // reliably driven, so we were sampling a floating pin. A pull-up makes an
  // undriven line read recessive (HIGH), which is what "idle" means on CAN.
  // It cannot mask traffic: a real dominant bit is driven by a push-pull CMOS
  // output that wins against a ~45 kOhm pull-up, and it cannot hold the pin
  // low, which was the original (mistaken) reason for disabling it.
  gpio_set_pull_mode((gpio_num_t)CAN_RX_GPIO, GPIO_PULLUP_ONLY);
#if BTN_NEXT_ACTIVE_LOW
  // The button can wake us too. It idles HIGH through its pull-up and pulls
  // LOW when pressed - the same polarity as an idle-recessive CAN line - so
  // both pins go in one ext1 mask under ANY_LOW. Handy in the car: wake the
  // gauge to read tyre pressures without starting the engine.
  gpio_set_pull_mode((gpio_num_t)BTN_NEXT_GPIO, GPIO_PULLUP_ONLY);
#endif
  delayMicroseconds(50);                // let the pads settle before sampling

  // Only arm a pin that is actually high right now. A pin already low would
  // trigger ANY_LOW the instant we slept - a held or stuck button must not be
  // able to spin the board on boot-sleep-boot.
  const bool chattering = fruitlessWakes > CAN_WAKE_FRUITLESS_MAX;

  uint64_t mask = 0;
  const int canLevel = gpio_get_level((gpio_num_t)CAN_RX_GPIO);
  // The backoff drops the CAN pin ONLY, because the CAN pin is the only thing
  // that can chatter.
  const bool canArmed = (canLevel == 1) && !chattering;
  if (canArmed) mask |= 1ULL << CAN_RX_GPIO;

  bool btnArmed = false;
#if BTN_NEXT_ACTIVE_LOW
  // The button is armed whenever it idles high, chatter or not. It cannot
  // chatter on its own - it goes low because a person pressed it.
  //
  // ⚠ This used to be gated on the same `chattering` flag as the CAN pin, via
  // a single useEdge, and that made the backoff SELF-LATCHING: once ext1 was
  // disarmed the button could no longer wake the board, and a button wake is
  // one of only two things that clear fruitlessWakes (the other being a live
  // bus). So after CAN_WAKE_FRUITLESS_MAX noise wakes the counter could never
  // come down, and the board stayed in timer-only mode for good - waking every
  // SLEEP_BACKOFF_SEC, blinking, sleeping, and ignoring the button entirely.
  // Observed on the bench: "it blinked once, then no more waking by button".
  const int btnLevel = gpio_get_level((gpio_num_t)BTN_NEXT_GPIO);
  btnArmed = (btnLevel == 1);
  if (btnArmed) mask |= 1ULL << BTN_NEXT_GPIO;
#endif

  const bool useEdge = (mask != 0);

  // Motion (QMI8658 INT2 -> GPIO21, ext0) - armed after bsp_prepare_sleep()
  // below, but decided here so the log says so. Off by setting, or after too
  // many motion wakes in a row that found no car.
  const bool motionWanted = settings.motionWakeMg != 0 &&
                            motionFruitless <= MOTION_FRUITLESS_MAX;

  // Log the decision BEFORE tearing anything down. Deep sleep cuts
  // USB-Serial-JTAG dead, and flush() only drains our own side, so a line
  // printed immediately before esp_deep_sleep_start() is regularly lost -
  // which is exactly the line you need when the sleep choice is wrong.
  // Name the two pins separately. "edge (CAN/button)" could not distinguish
  // "both armed" from "only the button armed", which is exactly the state the
  // chatter backoff produces and exactly what needed to be visible.
  const char *edges = canArmed  && btnArmed ? "edge: CAN + button"
                    : canArmed               ? "edge: CAN only"
                    : btnArmed && chattering ? "edge: button only (CAN chattering)"
                    : btnArmed               ? "edge: button only (CAN not idle high)"
                                             : "timer only (no pin idle high)";
  // Diagnostics: empty motion wakes in a row, and how long the sleep that
  // ended in this boot lasted (slept=0s = a false re-wake right after sleep).
  char slept[16] = "-";
  if (lastSleepSec != UINT32_MAX) snprintf(slept, sizeof(slept), "%lus", (unsigned long)lastSleepSec);
  char decision[128];
  snprintf(decision, sizeof(decision), "%s%s (mN=%lu slept=%s)", edges,
           motionWanted ? " + motion"
           : settings.motionWakeMg ? " (motion off: too many empty wakes)" : "",
           (unsigned long)motionFruitless, slept);
  const int wakeCause = (int)esp_sleep_get_wakeup_cause();
  Serial.printf("[..] sleep: wakeCause=%d canRx=%d mask=0x%llx fruitless=%lu -> %s\n",
                wakeCause, canLevel, (unsigned long long)mask,
                (unsigned long)fruitlessWakes, decision);
  Serial.flush();

  // To the card as well: in the dash there is no serial console, and this is
  // the only record of what parked behaviour actually happened.
  logger_note_sleep(decision, wakeCause, canLevel, (unsigned long)fruitlessWakes);

  // And to NVS, which is the only one of the three that survives having no SD
  // card AND a board that sleeps faster than the screen can be read.
  {
    const SleepRec rec = {
      .awakeMs   = millis(),
      .mask      = (uint32_t)mask,
      .fruitless = (uint16_t)fruitlessWakes,
      .wakeCause = (uint8_t)wakeCause,
      .canLevel  = (uint8_t)canLevel,
    };
    settings_log_sleep(&rec);
  }

#if SLEEP_SHOW_REASON
  // And to the SCREEN, held long enough to read.
  //
  // Both existing channels are unavailable exactly when this matters: on buck
  // power there is no serial host, and often no SD card either. That left the
  // board going black with no way to find out why - which is precisely the
  // state this was added to diagnose, after "black after half a second" had
  // produced three wrong theories in a row.
  //
  // wakeCause 3 = EXT1 (a pin went low), 4 = timer. canRx is the RX level
  // sampled with a pull-up, and mask shows which pins were actually armed.
  {
    char note[160];
    snprintf(note, sizeof(note),
             "sleeping\n%s\nwakeCause=%d canRx=%d\nmask=0x%llx fruitless=%lu",
             decision, wakeCause, canLevel,
             (unsigned long long)mask, (unsigned long)fruitlessWakes);
    ui_show_notice(note);
    delay(SLEEP_SHOW_REASON_MS);
  }
#endif

  // A freshly updated firmware that got this far works: confirm it now. A
  // deep-sleep wake is a reboot, and an unconfirmed image that reboots is
  // rolled back by the bootloader - see ota.h.
  ota_confirm();

  gps_park();            // 17.2 mA of a 24 mA parked draw - the biggest single
                         // item; see GPS_SLEEP_ON_PARK in config.h
  // Transceiver to standby: ~2.8 mA. Deliberately AFTER the mask was sampled
  // above, so the CAN RX level that decides edge-vs-timer wake is read with
  // the transceiver in full high-speed mode. Standby keeps the receiver alive,
  // so the armed mask stays valid - see CAN_RS_CONTROL in config.h.
  obd_transceiver_standby(true);
  web_stop();            // AP is normally up the whole time we're awake
  logger_close();        // FATFS caches - sleeping with the file open loses rows
  bsp_prepare_sleep();   // display, IMU, touch, SD - see the note in bsp.cpp

  if (useEdge) {
    // Keep the pull-ups through sleep, for the same reason we sampled with
    // them: an undriven line must read recessive, or ANY_LOW fires immediately
    // and we spin. A dominant bit, or a finger on the button, still overrides.
    for (int pin = 0; pin < 64; pin++) {
      if (!(mask & (1ULL << pin))) continue;
      rtc_gpio_pullup_en((gpio_num_t)pin);
      rtc_gpio_pulldown_dis((gpio_num_t)pin);
    }
    esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);
  }

  // Motion, on its own ext0 source: INT2 TOGGLES on every detection rather
  // than pulsing (bench, 2026-10-08), so it can rest high or low, and ext1's
  // single ANY_LOW for the whole mask cannot serve it. Arm the sensor, let
  // the pin settle, then wake on the OPPOSITE of wherever it rests.
  // bsp_prepare_sleep() powered the IMU down; configWakeOnMotion() resets it
  // into low-power wake-on-motion, and bsp_init() resets it again on wake.
  if (motionWanted && bsp_imu_wom_arm(settings.motionWakeMg)) {
    // After an ext0 (motion) wake, GPIO21 is still routed to the RTC IO
    // mux, and the digital gpio_get_level() below can then read the wrong
    // level - arming ext0 for the level the pin already sits at, so the pod
    // woke the instant it slept. Seen in the car 2026-10-08: only the sleep
    // AFTER a motion wake re-woke at once, never the first one. Hand the pin
    // back to the digital GPIO matrix first, as ESP-IDF says to after a wake.
    rtc_gpio_deinit((gpio_num_t)MOTION_WAKE_GPIO);
    pinMode(MOTION_WAKE_GPIO, INPUT);
    delay(MOTION_ARM_SETTLE_MS);
    // Then insist on a STABLE level: the first bench run slept, and was woken
    // by "motion" one second later with nobody touching it - INT2 had flipped
    // once more after the settle sample. Re-sample until it holds still for
    // MOTION_ARM_STABLE_MS (bounded, so a really moving pod still sleeps).
    int level = gpio_get_level((gpio_num_t)MOTION_WAKE_GPIO);
    for (uint32_t t0 = millis(), since = millis();
         millis() - t0 < 4 * MOTION_ARM_STABLE_MS; delay(5)) {
      const int l = gpio_get_level((gpio_num_t)MOTION_WAKE_GPIO);
      if (l != level) { level = l; since = millis(); }
      else if (millis() - since >= MOTION_ARM_STABLE_MS) break;
    }
    bsp_imu_status1();                    // clear the sensor's motion latch
    rtc_gpio_pullup_dis((gpio_num_t)MOTION_WAKE_GPIO);    // INT2 is push-pull
    rtc_gpio_pulldown_dis((gpio_num_t)MOTION_WAKE_GPIO);
    esp_sleep_enable_ext0_wakeup((gpio_num_t)MOTION_WAKE_GPIO, level ? 0 : 1);
  }

  // The timer follows the CAN pin, not the mask.
  //
  // With CAN armed the timer is only a safety net, so it can be long. With CAN
  // dropped - chattering, or the line not idling high - it is the only thing
  // that will ever re-check the bus, so it has to be the short backoff even
  // though the button is still armed. Keying this off `useEdge` would have
  // left a chattering board asleep for SLEEP_SAFETY_SEC between bus checks,
  // now that the button alone keeps useEdge true.
  esp_sleep_enable_timer_wakeup(
      (uint64_t)(canArmed ? SLEEP_SAFETY_SEC : SLEEP_BACKOFF_SEC) * 1000000ULL);

  sleptAtUs = esp_rtc_get_time_us();   // read back by the next boot
  esp_deep_sleep_start();  // never returns; boots fresh into setup()
}

// A cold boot means a human just applied power - on the bench, or plugging the
// pod into the car. A timer/edge wake does not. Only the latter should blank
// the screen a second and a half in.
// Set by power_boot_check() on a deep-sleep wake, resolved by power_task().
static bool     wakeSampling = false;
static uint32_t wakeSampleT0 = 0;
static bool     wakeByCan    = false;   // the CAN line, not a timer, woke us
static bool     wakeByMotion = false;   // the IMU did (ext0)

static bool wokeFromSleep() {
  return esp_reset_reason() == ESP_RST_DEEPSLEEP;
}

void power_boot_check() {
#if DEV_DISABLE_SLEEP
  return;                               // bench mode: stay awake with no bus
#else
  // How long the sleep that just ended lasted - for the next sleep line.
  // Only meaningful on a deep-sleep wake: a cold boot resets the RTC clock.
  if (wokeFromSleep() && sleptAtUs != 0)
    lastSleepSec = (uint32_t)((esp_rtc_get_time_us() - sleptAtUs) / 1000000ULL);

  if (usbAttached()) {
    Serial.println(F("[..] USB host seen - sleep inhibited for this power cycle"));
    return;
  }

  // Someone just applied power and is presumably looking at the screen. Going
  // dark 1.5 s later is indistinguishable from a dead display - which is
  // exactly how this was first reported, on a bench supply with no CAN bus.
  // power_task() still sleeps once COLD_BOOT_AWAKE_MS expires, so this costs
  // one window per plug-in, not standing current.
  if (!wokeFromSleep()) {
    Serial.printf("[..] cold boot - awake %d ms even with no bus\n",
                  COLD_BOOT_AWAKE_MS);
    return;
  }

  // A button press is a person asking for the screen, so treat it exactly like
  // a cold boot rather than sampling the bus and going straight back to sleep.
  // Without this, pressing the button woke the board, found no CAN traffic and
  // slept again about 1.5 s later - the display barely flickered, which looked
  // like the button not working at all. ext1 latches which pin fired, so this
  // still reads correctly after the button is released.
  if ((esp_sleep_get_ext1_wakeup_status() & (1ULL << BTN_NEXT_GPIO)) != 0) {
    fruitlessWakes = 0;                 // asked for deliberately; not fruitless
    motionFruitless = 0;
    Serial.printf("[..] woken by button - awake %d ms\n", COLD_BOOT_AWAKE_MS);
    return;
  }

  // Watch for the bus AND for USB together, for WAKE_SAMPLE_MS - from
  // power_task(), in loop(), not here. Blocking here held setup() for up to
  // 1.5 s on every car start (every one is a deep-sleep wake), and the boot
  // splash stood frozen on its first frame for all of it (2026-10-04: seen
  // in the car, never on the bench, where USB returns above at once).
  //
  // Sampling USB only once at the top of this function was a race, and we lost
  // it: isPlugged() is SOF-based, and immediately after a deep-sleep wake the
  // host is not clocking the bus yet - the device has only just pulled D+ up,
  // and re-enumeration takes a few hundred milliseconds. So a board woken with
  // the cable still plugged in decided there was no USB, found no CAN traffic,
  // and slept again about a second later. The kernel log showed it precisely:
  // "new full-speed USB device" and "USB disconnect" inside the same second,
  // repeatedly - while the panel was lit often enough to look like the board
  // was simply awake and ignoring USB.
  //
  // We are already waiting for the bus, so checking USB in the same check
  // costs nothing and closes the race.
  wakeSampling = true;
  wakeSampleT0 = millis();
  // Remember whether the CAN line is what woke us: only such a wake that
  // finds no traffic is evidence of chatter - see power_task().
  wakeByCan = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1 &&
              (esp_sleep_get_ext1_wakeup_status() & (1ULL << CAN_RX_GPIO)) != 0;
  // A motion wake (door, someone getting in) comes well BEFORE the bus:
  // power_task() waits MOTION_WAKE_SAMPLE_MS for it instead of WAKE_SAMPLE_MS.
  wakeByMotion = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0;
  if (wakeByMotion) Serial.printf("[..] woken by motion after %lu s asleep - waiting for the bus\n",
                                  (unsigned long)lastSleepSec);
#endif
}

bool power_wake_pending() { return wakeSampling; }

// For the gauge's state (podstate.cpp).
bool power_usb_host() { return usbAttached(); }
uint32_t power_last_activity_ms() { return lastActivityMs; }
uint32_t power_sleep_after_ms() { return idleSleepMs(); }

PsWake power_wake_cause() {
  if (!wokeFromSleep()) return PS_WAKE_COLD;
  const esp_sleep_wakeup_cause_t c = esp_sleep_get_wakeup_cause();
  if (c == ESP_SLEEP_WAKEUP_EXT0) return PS_WAKE_MOTION;
  if (c == ESP_SLEEP_WAKEUP_EXT1)
    return (esp_sleep_get_ext1_wakeup_status() & (1ULL << BTN_NEXT_GPIO)) ? PS_WAKE_BUTTON : PS_WAKE_CAN;
  return PS_WAKE_TIMER;
}

void power_task() {
#if DEV_DISABLE_SLEEP
  return;
#else
  // A deep-sleep wake still deciding whether the car is really on - see
  // power_boot_check(). Nothing else decides about sleep meanwhile.
  if (wakeSampling) {
    if (usbAttached()) {
      Serial.println(F("[..] USB host seen on wake - sleep inhibited"));
      wakeSampling = false;
    } else if (podstate().awakeBecause & PS_AWAKE_MOVING) {
      // Woken, and the car is moving with no bus: a drive with a CAN fault,
      // not a parked car. Up, and the ordinary rules below take over.
      Serial.println(F("[..] moving with no CAN traffic - staying awake"));
      wakeSampling = false;
    } else if (obd.lastFrameMs != 0) {
      fruitlessWakes = 0;               // a real drive: forget the backoff
      motionFruitless = 0;
      wakeSampling = false;             // bus is alive -> stay up
    } else if (calc_elapsed(millis(), wakeSampleT0,
                            wakeByMotion ? MOTION_WAKE_SAMPLE_MS : WAKE_SAMPLE_MS)) {
      // Only a CAN-caused wake that found nothing counts as chatter. This
      // used to count EVERY empty wake - including the hourly safety-timer
      // wakes, which are supposed to find nothing while parked - so after
      // ~6 h parked the pod "detected chatter", disarmed CAN wake and fell
      // back to the 5-minute timer: then a car start took up to 5 min to
      // wake it (2026-10-07 sleeplog: 188 timer wakes, 4 CAN wakes - all
      // real drives - and 0 actual empty CAN wakes).
      // A timer wake that finds nothing resets the count instead, so a real
      // chatter backoff re-arms CAN after one backoff period rather than
      // never (it used to clear only on a drive or the button).
      // Motion wakes keep their own count (MOTION_FRUITLESS_MAX), so wind
      // cannot disarm CAN wake and CAN chatter cannot disarm motion. A timer
      // wake that finds nothing resets both.
      if (wakeByMotion)   motionFruitless++;
      else if (wakeByCan) fruitlessWakes++;
      else              { fruitlessWakes = 0; motionFruitless = 0; }
      goToSleep();
    }
    return;
  }

  // Awake, or asleep: the gauge's state (podstate.h, since 2026-10-24) -
  // nothing holding it off any more (awakeBecause, the drive CSV's `awake`):
  // a USB host, a phone on the WiFi or one just gone, a touch or the button,
  // moving, the car talking, a cold boot that has heard no car yet - each for
  // the "Sleep after" window after it ends. The rules and why are there, worked
  // out at the top of this pass. These decided here, one by one, until then;
  // the state was checked against them first, on real drives - it would have
  // slept at the same second.
  if (podstate().sleepOk) goToSleep();
#endif
}
