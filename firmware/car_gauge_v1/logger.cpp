// ============================================================================
// logger.cpp  —  the logger task and the SD card: mounting, the lock, closing,
// the sleep log. The writers are log_csv, log_gpx and log_vbo - see
// log_priv.h.
// ============================================================================
#include "logger.h"
#include "log_priv.h"
#include "config.h"
#include "calc.h"       // calc_gear
#include "obd.h"
#include "gps.h"
#include "rtc.h"        // clock_valid(): asks the clock, not one of its sources
#include "settings.h"   // logLevel, and the key generation
#include "SD_MMC.h"

// FAT file times in UTC, not local time. FatFs asks get_fattime() for the
// stamp, and ESP-IDF's version uses localtime() - which is what the FAT spec
// says, but Linux (and Android) mount vfat reading the stamps as UTC, so on
// the PC every file showed 2 h late in CEST (2026-10-04: a log whose last row
// was 15:12:53Z, 17:12 local, listed as 19:12). The card is read on Linux, so
// stamp UTC. Windows would now show them 2 h early.
//
// Reached through the linker: scripts/build.sh passes -Wl,--wrap=get_fattime,
// which sends FatFs's call here instead of to diskio.c (defining a plain
// get_fattime would collide with it, as that object also holds the disk I/O
// FatFs needs). A build without the flag - the Arduino IDE - just leaves this
// unused and keeps local-time stamps.
extern "C" uint32_t __wrap_get_fattime(void) {
  const time_t t = time(nullptr);
  struct tm tm;
  gmtime_r(&t, &tm);
  if (tm.tm_year < 80) return (1u << 21) | (1u << 16);   // no clock: 1980-01-01
  return ((uint32_t)(tm.tm_year - 80) << 25) | ((uint32_t)(tm.tm_mon + 1) << 21) |
         ((uint32_t)tm.tm_mday << 16) | ((uint32_t)tm.tm_hour << 11) |
         ((uint32_t)tm.tm_min << 5) | ((uint32_t)tm.tm_sec >> 1);
}

static bool cardOk = false;     // card mounted; a file may not be open yet
static bool bufsOk = false;     // every writer has its batch: logging may start

bool logger_card_ok()   { return cardOk; }
bool logger_write_failed() {
  return cardOk && (csvlog_failed() || gpxlog_failed() || vbolog_failed());
}

// ---- idle throttle --------------------------------------------------------
static uint32_t lastMovingMs = 0;

// True once the car has been stationary a while. Deliberately returns false
// when there is no GPS fix: without one we cannot tell standing still from
// driving through a tunnel, and quietly thinning the log would be worse than
// writing a few redundant rows. That also covers sitting in a garage doing
// CAN work with no sky view - full rate, which is what you want there.
bool log_idle() {
  if (!gps.haveFix) return false;
  if (gps.speedKmh >= LOG_IDLE_SPEED_KMH) {
    lastMovingMs = millis();
    return false;
  }
  return (millis() - lastMovingMs) > LOG_IDLE_AFTER_MS;
}

int log_gear() {
  static const float ratios[] = GEAR_RPM_PER_KMH;
  if (!obd_rpm_fresh() || !obd_speed_fresh()) return -1;
  return calc_gear((float)obd.rpm, (float)obd.speedKmh, ratios,
                   sizeof(ratios) / sizeof(ratios[0]), GEAR_TOL, GEAR_MIN_KMH);
}

// ---- the SD lock -------------------------------------------------------------
// The logger runs on its own task on core 0 (logger_start), so the screen on
// core 1 never waits for the card - a flush costs 13 ms once settled and up
// to ~460 ms when the card stalls (measured 2026-10-11). Everything else that
// touches the card from core 1 - sleep, the USB share, the phone's file
// pages, the serial dump - takes this lock first, and so waits at most one
// save. Recursive, because the public functions call each other.
static SemaphoreHandle_t sdLock = nullptr;
void logger_sd_lock()   { if (sdLock) xSemaphoreTakeRecursive(sdLock, portMAX_DELAY); }
void logger_sd_unlock() { if (sdLock) xSemaphoreGiveRecursive(sdLock); }

// Set by logger_close() (sleep, reboot): the task stops opening files, so
// nothing reopens one between the close and the power going.
static volatile bool stopped = false;

// Same 1-bit SDMMC wiring as the sniffer sketch: CLK=2, CMD=1, D0=3.
#define SD_CLK 2
#define SD_CMD 1
#define SD_D0  3

void logger_init() {
  if (!sdLock) sdLock = xSemaphoreCreateRecursiveMutex();
  // The batches in PSRAM (see log_sink.cpp), once, before any writer runs.
  // Without them logging stays off, and the "no SD card" fault says nothing
  // is being logged.
  bufsOk = csvlog_init() && gpxlog_init() && vbolog_init();
  if (!bufsOk) {
    Serial.println(F("[ERR] no PSRAM for the log buffers - drive logging disabled"));
    return;
  }
  lognames_new_session();
  if (!SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0)) {
    Serial.println(F("[ERR] SD_MMC.setPins failed - no drive log"));
    return;
  }
  if (!SD_MMC.begin("/sdcard", /*1-bit*/true)) {
    Serial.println(F("[..] no SD card - drive logging disabled"));
    return;
  }
  cardOk = true;
  lastMovingMs = millis();   // don't count as idle before we've ever moved
  Serial.println(F("[OK] SD card mounted"));
  // Deliberately NO file yet - see log_csv.cpp's driveOpen().
}

// The card is lent to the PC as a USB drive (usbdisk.cpp): close everything
// and stop touching it. cardOk=false is what every writer here already
// checks, so this one flag stops the CSV, the GPX, the VBO and the sleeplog;
// webapi.cpp checks logger_card_ok() before listing or serving files.
static bool sharedWithPc = false;

static void closeAll() {
  vbolog_close();                // a session resumes into a new file after sleep
  gpxlog_close();
  csvlog_close();
}

void logger_suspend() {
  LoggerSdLock lock;
  closeAll();
  cardOk = false;
  sharedWithPc = true;
}

// The card is back. No remount: the PC had it read-only (usbdisk.cpp), so
// FATFS's cached view still holds - and unmounting while a last read from the
// PC might still be in disk_read() is a race not worth having. Logging
// resumes in a new file on its own.
void logger_resume() {
  LoggerSdLock lock;
  sharedWithPc = false;
  cardOk = bufsOk;   // never without the buffers to write through
  // The PC may have freed space: every writer may try its file again.
  csvlog_card_back();
  gpxlog_card_back();
  vbolog_card_back();
  Serial.println(F("[OK] SD card back"));
}

bool logger_card_shared() { return sharedWithPc; }

static void loggerTask() {
  // A level change from the phone closes every open file, so it applies at
  // once rather than at the next drive; whatever the new level allows opens
  // again as a new file.
  static uint8_t lastLevel = settings.logLevel;
  if (settings.logLevel != lastLevel) {
    Serial.printf("[..] log level %u -> %u\n", lastLevel, settings.logLevel);
    lastLevel = settings.logLevel;
    closeAll();
  }
  // Same for the encryption key: set, changed or removed, the open files
  // end and the next ones start under the new key.
  static uint32_t lastKeyGen = settings_log_key_gen;
  if (settings_log_key_gen != lastKeyGen) {
    lastKeyGen = settings_log_key_gen;
    Serial.printf("[..] log encryption %s\n", settings.logKey ? "key set" : "off");
    closeAll();
  }
  logsink_stir();

  gpxlog_task();             // its own cadence, independent of the CSV
  if (cardOk && settings.logLevel >= LOG_TRIPS)
    vbolog_task();           // per GNSS fix, like the GPX - not the CSV's rate

  if (!cardOk) return;
  if (settings.logLevel < LOG_NORMAL) return;          // no drive CSV
  csvlog_task();
}

// The logger's own task, on core 0 at priority 1: below CAN (5) and GPS (4),
// which preempt it the moment they have work, and the time a save costs is
// mostly the card working, not this core. A pass every LOGGER_PASS_MS, the
// pace loop() used to give it; the delay also lets core 0's idle task run.
static void loggerTaskLoop(void *) {
  for (;;) {
    {
      LoggerSdLock lock;
      if (!stopped) {
        const uint32_t t0 = micros();
        loggerTask();
        csvlog_note_pass_us(micros() - t0);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(LOGGER_PASS_MS));
  }
}

void logger_start() {
  static bool started = false;
  if (started || !sdLock) return;
  started = true;
  xTaskCreatePinnedToCore(loggerTaskLoop, "logger", 8192, NULL, 1, NULL, 0);
}

void logger_note_sleep(const char *decision, int wakeCause, int canRx,
                       unsigned long fruitless) {
  LoggerSdLock lock;
  if (!cardOk) return;

  // Bounded: one line per sleep is tiny, but parked on the backoff timer this
  // appends a few hundred lines a night, forever. Stop growing rather than
  // silently filling the card that the drive logs need.
  if (SD_MMC.exists(SLEEP_LOG_PATH)) {
    File probe = SD_MMC.open(SLEEP_LOG_PATH, FILE_READ);
    if (probe) {
      const size_t sz = probe.size();
      probe.close();
      if (sz > SLEEP_LOG_MAX_BYTES) return;
    }
  }

  File f = SD_MMC.open(SLEEP_LOG_PATH, FILE_APPEND);
  if (!f) return;
  char utc[24] = "no-clock";
  if (clock_valid()) {
    const time_t t = time(nullptr);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(utc, sizeof(utc), "%Y-%m-%dT%H:%M:%SZ", &tm);
  }
  // awakeMs says how long this wake cost us - the number that decides whether
  // parked current is acceptable.
  f.printf("%s awakeMs=%lu wakeCause=%d canRx=%d fruitless=%lu %s\n",
           utc, (unsigned long)millis(), wakeCause, canRx, fruitless, decision);
  f.flush();
  f.close();
}

// Before sleep or a reboot: close everything and stop for good - the task
// must not open a new file between this and the power going.
void logger_close() {
  LoggerSdLock lock;
  closeAll();
  stopped = true;
}

// Is this card file (a plain name, no slash) one the logger is writing to,
// or will write to again this session? /api/delete refuses those. Read
// under LoggerSdLock: the paths change only while it is held.
bool logger_is_open(const char *name) {
  for (const char *p : {csvlog_path(), gpxlog_path(), vbolog_path()})
    if (p[0] == '/' && strcmp(p + 1, name) == 0) return true;
  return false;
}
