// ============================================================================
// ota.cpp  —  see ota.h.
// ============================================================================
#include "ota.h"
#include "config.h"
#include "logger.h"     // logger_close() before a reboot
#include "ui.h"         // ui_show_notice() during an SD install
#include "calc.h"       // calc_mark_scan()
#include <Arduino.h>
#include <Update.h>
#include "FS.h"
#include "SD_MMC.h"
#include <Preferences.h>     // the hash of the last SD file acted on
#include <memory>
#include <new>
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"

static char lastError[64] = "";
static bool probation = false;          // running image not yet confirmed

// Keep the core from confirming a probation boot during startup: we do it
// ourselves, once the firmware has shown it actually runs. C linkage - the
// weak default lives in the core's esp32-hal-misc.c.
extern "C" bool verifyRollbackLater() { return true; }

// FW_BUILD comes from build_info.h, written by scripts/build.sh on every
// build: date, time and git commit ("+changes" if uncommitted). A build
// without the script (Arduino IDE) falls back to the compile time.
#if __has_include("build_info.h")
#include "build_info.h"
#endif
#ifndef FW_BUILD
#define FW_BUILD __DATE__ " " __TIME__
#endif
const char *ota_build() { return FW_BUILD; }

bool ota_rolled_back() {
  // The bootloader marks the image it gave up on; ESP-IDF reports it here.
  return esp_ota_get_last_invalid_partition() != NULL;
}

bool ota_on_probation() { return probation; }

static void noteProbation() {
  static bool checked = false;
  if (checked) return;
  checked = true;
  esp_ota_img_states_t st;
  const esp_partition_t *run = esp_ota_get_running_partition();
  probation = run && esp_ota_get_state_partition(run, &st) == ESP_OK &&
              st == ESP_OTA_IMG_PENDING_VERIFY;
  if (probation)
    Serial.printf("[..] new firmware on probation in %s - confirming after %u s\n",
                  run->label, (unsigned)(OTA_HEALTHY_MS / 1000));
  if (ota_rolled_back())
    Serial.println(F("[ERR] a firmware update was rolled back - the previous one is running"));
}

void ota_confirm() {
  noteProbation();
  if (!probation) return;
  if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
    probation = false;
    Serial.println(F("[OK] new firmware confirmed"));
  }
}

void ota_task() {
  noteProbation();
#ifdef OTA_CRASH_TEST
  // Rollback test only: a firmware that dies before it can confirm itself.
  // Built with -DOTA_CRASH_TEST, never committed enabled.
  if (millis() > 5000) { Serial.println(F("[TEST] crashing on purpose")); abort(); }
#endif
  if (probation && millis() >= OTA_HEALTHY_MS) ota_confirm();
}

// ---- writing an image ------------------------------------------------------
// Only this gauge's firmware is installed. The bootloader checks that an
// image is valid for the chip, nothing more: any ESP32-S3 sketch would be
// taken, and a wrong .bin picked on the phone would replace the gauge with
// something that cannot take an update back. The image's own project name
// does not tell them apart - it is "arduino-lib-builder" for every Arduino
// sketch - so the firmware carries this string, and an image is accepted only
// if it carries it too. The scan below is what keeps it in the image. Guards
// against a mistake, not an attacker: anyone can put the string in a file.
static const char FIRMWARE_MARK[] = "N-Gauge firmware image";
static CalcMarkScan markScan;

bool ota_begin(size_t total) {
  lastError[0] = '\0';
  markScan = CalcMarkScan();
  if (!Update.begin(total ? total : UPDATE_SIZE_UNKNOWN, U_FLASH)) {
    snprintf(lastError, sizeof(lastError), "begin: %s", Update.errorString());
    return false;
  }
  return true;
}

bool ota_write(const uint8_t *data, size_t len) {
  calc_mark_scan(&markScan, FIRMWARE_MARK, data, len);
  if (Update.write((uint8_t *)data, len) != len) {
    snprintf(lastError, sizeof(lastError), "write: %s", Update.errorString());
    Update.abort();
    return false;
  }
  return true;
}

bool ota_end() {
  if (!markScan.seen) {
    snprintf(lastError, sizeof(lastError), "not N Gauge firmware");
    Update.abort();
    return false;
  }
  // end(true): accept the size written so far as the image size. Update
  // checks the image header and its SHA-256 before switching the boot slot.
  if (!Update.end(true)) {
    snprintf(lastError, sizeof(lastError), "end: %s", Update.errorString());
    return false;
  }
  return true;
}

void ota_abort() { Update.abort(); }
const char *ota_error() { return lastError; }

void ota_reboot() {
  logger_close();               // a reboot without this loses buffered rows
  delay(300);
  ESP.restart();
}

// ---- identifying an image ----------------------------------------------------
// Every ESP32 app image carries an esp_app_desc_t right after the image
// header and the first segment header, with the SHA-256 of the ELF it was
// built from. Reading it from the file costs ~300 bytes; nothing is hashed.
// Any change to the code - or to build_info.h - gives a different value.
static const size_t APP_DESC_OFFSET =
    sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);

static bool fileAppHash(File &f, uint8_t out[32]) {
  esp_app_desc_t d;
  if (!f.seek(APP_DESC_OFFSET) || f.read((uint8_t *)&d, sizeof(d)) != sizeof(d)) return false;
  if (d.magic_word != ESP_APP_DESC_MAGIC_WORD) return false;
  memcpy(out, d.app_elf_sha256, 32);
  return true;
}

static void hexShort(char *out, const uint8_t *h) {   // first 4 bytes, 8 chars
  snprintf(out, 9, "%02x%02x%02x%02x", h[0], h[1], h[2], h[3]);
}

const char *ota_hash() {
  static char s[9] = "";
  if (!s[0]) hexShort(s, esp_app_get_description()->app_elf_sha256);
  return s;
}

// ---- from the SD card ------------------------------------------------------
// Each file is acted on ONCE, by its hash: the hash of the last file looked
// at is kept in NVS, and a file with that hash is skipped. Written BEFORE
// installing, so a file that installs, then crashes and is rolled back, is
// not installed again on the next boot - no loop. It also means a firmware
// flashed over USB later is never overwritten by an old file still on the
// card. The file stays where it is, so to update again, put a new build
// there (any change gives a new hash). Replaces the old rename to
// firmware_installed.bin / firmware_failed.bin (2026-10-09).
void ota_check_sd() {
  if (!SD_MMC.exists(OTA_SD_PATH)) return;
  File f = SD_MMC.open(OTA_SD_PATH, FILE_READ);
  if (!f || f.isDirectory() || f.size() == 0) { if (f) f.close(); return; }

  uint8_t fileHash[32];
  char fileShort[9];
  if (!fileAppHash(f, fileHash)) {
    f.close();
    Serial.printf("[ERR] %s is not an ESP32 app image - ignored\n", OTA_SD_PATH);
    return;
  }
  hexShort(fileShort, fileHash);

  Preferences p;
  p.begin("ota", /*readOnly=*/false);
  uint8_t seen[32] = { 0 };
  const bool haveSeen = p.getBytes("seen", seen, sizeof(seen)) == sizeof(seen);
  if (haveSeen && memcmp(seen, fileHash, 32) == 0) {
    p.end();
    f.close();
    Serial.printf("[..] %s (%s) already handled - running %s\n",
                  OTA_SD_PATH, fileShort, ota_hash());
    return;
  }
  p.putBytes("seen", fileHash, sizeof(fileHash));   // before anything can fail
  p.end();

  if (memcmp(fileHash, esp_app_get_description()->app_elf_sha256, 32) == 0) {
    f.close();
    Serial.printf("[..] %s (%s) is the running firmware - nothing to do\n",
                  OTA_SD_PATH, fileShort);
    return;
  }

  const size_t size = f.size();
  f.seek(0);
  Serial.printf("[..] installing %s (%s, %u bytes) from the SD card\n",
                OTA_SD_PATH, fileShort, (unsigned)size);
  ui_show_notice("Updating firmware\nfrom the SD card...");

  // Borrowed for the install and given back, not a static array: that held
  // 4 KB of internal RAM for good, from every boot, for an update that is
  // almost never there - RAM the WiFi AP is short of (AP_MIN_INTERNAL_HEAP).
  // Internal, as the card reads into it by DMA.
  constexpr size_t BUF_N = 4096;
  std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[BUF_N]);
  if (!buf) snprintf(lastError, sizeof(lastError), "no RAM for the copy buffer");
  bool ok = buf && ota_begin(size);
  while (ok && f.available()) {
    const size_t n = f.read(buf.get(), BUF_N);
    if (n == 0) break;
    ok = ota_write(buf.get(), n);
  }
  buf.reset();
  f.close();
  if (ok) ok = ota_end();
  else    ota_abort();

  if (!ok) {
    Serial.printf("[ERR] SD update failed: %s\n", ota_error());
    char msg[96];
    snprintf(msg, sizeof(msg), "Update failed\n%s", ota_error());
    ui_show_notice(msg);
    delay(3000);
    ui_show_notice(NULL);
    return;                     // keep running the current firmware
  }
  Serial.println(F("[OK] SD update installed - rebooting"));
  ui_show_notice("Update installed\nrestarting...");
  delay(1000);
  SD_MMC.end();
  ESP.restart();
}
