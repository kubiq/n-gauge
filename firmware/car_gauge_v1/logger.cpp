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
#include "esp_task_wdt.h"   // fed while waiting for the SD lock

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
// Writes failed and remounting did not help (SD_REMOUNT_MAX): the writers
// stay off, the card stays up for the phone. Cleared when it comes back from
// the PC, which may have freed space.
static bool writesOff = false;

bool logger_card_ok()   { return cardOk; }
bool log_may_write()    { return cardOk && !writesOff; }
bool logger_write_failed() {
  return cardOk && (writesOff || csvlog_failed() || gpxlog_failed() || vbolog_failed());
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

// Half a second at a time, the watchdog fed in between - the callers are on
// the loop's task, which it watches.
bool logger_sd_lock_for(uint32_t ms) {
  if (!sdLock) return true;          // before logger_init: nothing else uses the card
  const uint32_t t0 = millis();
  for (;;) {
    const uint32_t waited = millis() - t0;
    const uint32_t step = ms - waited < 500 ? ms - waited : 500;
    if (xSemaphoreTakeRecursive(sdLock, pdMS_TO_TICKS(step)) == pdTRUE) return true;
    esp_task_wdt_reset();
    if (millis() - t0 >= ms) {
      Serial.println(F("[ERR] SD card busy - a save has held it too long"));
      return false;
    }
  }
}

// Set by logger_close() (sleep, reboot): the task stops opening files, so
// nothing reopens one between the close and the power going.
static volatile bool stopped = false;

// Same 1-bit SDMMC wiring as the sniffer sketch: CLK=2, CMD=1, D0=3.
#define SD_CLK 2
#define SD_CMD 1
#define SD_D0  3

void logger_init() {
  if (!sdLock) sdLock = xSemaphoreCreateRecursiveMutex();
  // From setup(), and the AP only starts from loop(): the radio is not up yet.
  logsink_seed();
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

bool logger_suspend() {
  LoggerSdLock lock(SD_LOCK_WEB_MS);
  if (!lock) return false;           // mid-save on a stalled card: not now
  closeAll();
  cardOk = false;
  sharedWithPc = true;
  return true;
}

// The card is back. No remount: the PC had it read-only (usbdisk.cpp), so
// FATFS's cached view still holds - and unmounting while a last read from the
// PC might still be in disk_read() is a race not worth having. Logging
// resumes in a new file on its own.
//
// The lock without a limit: while the PC had the card, the logger task did
// not touch it (cardOk false), so it holds the lock for a moment at most.
void logger_resume() {
  LoggerSdLock lock;
  sharedWithPc = false;
  writesOff = false;
  cardOk = bufsOk;   // never without the buffers to write through
  // The PC may have freed space: every writer may try its file again.
  csvlog_card_back();
  gpxlog_card_back();
  vbolog_card_back();
  Serial.println(F("[OK] SD card back"));
}

bool logger_card_shared() { return sharedWithPc; }

// ---- a write that failed -----------------------------------------------------
// A loose card, a voltage dip, a card going bad: the writes come up short
// (logfile_check_write). The files are closed - each ends at its last good
// block, which every reader takes - the card is unmounted and mounted again,
// and the writers start new files. Mounting fails while the card is gone:
// tried again every SD_REMOUNT_RETRY_MS, the "no SD card" fault meanwhile.
static uint8_t  remounts = 0;
static bool     remountWanted = false;
static uint32_t remountTriedMs = 0;

static void cardRecover() {
  const uint32_t now = millis();
  if (remountTriedMs && now - remountTriedMs < SD_REMOUNT_RETRY_MS) return;
  remountTriedMs = now;
  closeAll();                        // what still reaches the card, does
  if (remounts >= SD_REMOUNT_MAX) {
    // Failing again after every remount is a full card, or a dying one:
    // stop writing rather than leave a new empty file every few seconds.
    Serial.println(F("[ERR] SD writes keep failing - logging stopped (full card?)"));
    writesOff = true;
    remountWanted = false;
    logsink_take_failure();
    return;
  }
  remounts++;
  SD_MMC.end();
  cardOk = SD_MMC.begin("/sdcard", /*1-bit*/true);
  logsink_take_failure();            // the closing writes' own failures
  if (!cardOk) {
    Serial.println(F("[ERR] SD card lost - trying again shortly"));
    remounts--;                      // a card being reseated is not a failing one
    return;
  }
  remountWanted = false;
  csvlog_card_back();
  gpxlog_card_back();
  vbolog_card_back();
  Serial.printf("[OK] SD card remounted (%u of %u)\n", remounts, SD_REMOUNT_MAX);
}

static void loggerTask() {
  if (logsink_take_failure()) {
    remountWanted = true;
    cardOk = false;                  // no writer touches it until it is back
  }
  if (remountWanted && !sharedWithPc) cardRecover();

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
  if (log_may_write() && settings.logLevel >= LOG_TRIPS)
    vbolog_task();           // per GNSS fix, like the GPX - not the CSV's rate

  if (!log_may_write()) return;
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

// One line, stamped with the UTC time (or "no-clock"), appended to a small
// plain-text log on the card - the sleep log and the boot log.
//
// Bounded: one line per sleep or boot is tiny, but parked on the backoff
// timer the sleep log gains a few hundred lines a night. A full one is moved
// aside to the first free numbered name ("/sleeplog.txt" ->
// "/sleeplog_0001.txt") and a new one started. It used to just stop growing -
// at 256 KB, about a week parked, so the record of every later sleep was lost.
// The moved ones are small and few (~13 MB a year); the app pulls each once.
static bool rotateFull(const char *path) {
  const char *dot = strrchr(path, '.');
  if (!dot) return false;
  char to[40];
  for (int k = 1; k <= 999; k++) {
    snprintf(to, sizeof(to), "%.*s_%04d%s", (int)(dot - path), path, k, dot);
    if (!SD_MMC.exists(to)) return SD_MMC.rename(path, to);
  }
  return false;
}

static void appendLine(const char *path, size_t maxBytes, const char *line) {
  LoggerSdLock lock(SD_LOCK_WEB_MS);
  if (!lock || !cardOk) return;      // a line lost, rather than the pod
  if (SD_MMC.exists(path)) {
    File probe = SD_MMC.open(path, FILE_READ);
    if (probe) {
      const size_t sz = probe.size();
      probe.close();
      // Can't move it aside: stop growing, as before, rather than fill the
      // card the drive logs need.
      if (sz > maxBytes && !rotateFull(path)) return;
    }
  }
  File f = SD_MMC.open(path, FILE_APPEND);
  if (!f) return;
  char utc[24] = "no-clock";
  if (clock_valid()) {
    const time_t t = time(nullptr);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(utc, sizeof(utc), "%Y-%m-%dT%H:%M:%SZ", &tm);
  }
  f.printf("%s %s\n", utc, line);
  f.flush();
  f.close();
}

void logger_note_sleep(const char *decision, int wakeCause, int canRx,
                       unsigned long fruitless) {
  // awakeMs says how long this wake cost us - the number that decides whether
  // parked current is acceptable.
  char line[200];
  snprintf(line, sizeof(line), "awakeMs=%lu wakeCause=%d canRx=%d fruitless=%lu %s",
           (unsigned long)millis(), wakeCause, canRx, fruitless, decision);
  appendLine(SLEEP_LOG_PATH, SLEEP_LOG_MAX_BYTES, line);
}

void logger_note_boot(const char *line) {
  appendLine(BOOT_LOG_PATH, BOOT_LOG_MAX_BYTES, line);
}

// Before sleep or a reboot: close everything and stop for good - the task
// must not open a new file between this and the power going.
//
// Stopped even when the lock never comes: the task sees it on its next pass.
// The files are then left as their last save had them - what a power cut
// would leave, which every format here is written to survive - and the pod
// still sleeps, instead of the watchdog rebooting it awake.
void logger_close() {
  stopped = true;
  LoggerSdLock lock(SD_LOCK_CLOSE_MS);
  if (lock) closeAll();
  else Serial.println(F("[ERR] logs not closed - the card stayed busy"));
}

// Is this card file (a plain name, no slash) one the logger is writing to,
// or will write to again this session? /api/delete refuses those. Read
// under LoggerSdLock: the paths change only while it is held.
bool logger_is_open(const char *name) {
  for (const char *p : {csvlog_path(), gpxlog_path(), vbolog_path()})
    if (p[0] == '/' && strcmp(p + 1, name) == 0) return true;
  return false;
}
