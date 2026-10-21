// ============================================================================
// log_sink.cpp  —  LogFile: a log file, its batch, and its encryption.
// See log_priv.h.
// ============================================================================
#include "log_priv.h"
#include "settings.h"   // logKey / logPub: whether, and to whom, to encrypt
#include "webapi.h"     // web_is_up(): the RNG is true-random with the radio on
#include <esp_random.h>
#include <esp_attr.h>           // RTC_DATA_ATTR
#include <bootloader_random.h>  // the RNG's entropy source with the radio off
#include <esp_heap_caps.h>
#include <stdarg.h>

// ---- encryption --------------------------------------------------------------
// With a key set (settings.logKey), the CSV, the GPX and the .vbo are written
// as logcrypt.h records - see there, and DECISIONS.md "Logs are to be
// encrypted to a public key". Each LogFile is plaintext as before with no
// key, otherwise a segment header when the file is opened and one sealed
// record per batch the writer already makes. The sleep log stays plain on
// purpose.
//
// The throwaway secret per segment needs real randomness. The S3's RNG is
// only guaranteed true-random while the radio runs (WiFi or BT), and the AP
// is up from boot but drops while driving (AP_DRIVE_OFF) - just when a GPX
// may open at the first fix. So RNG output is stirred into a pool on every
// pass with the AP up, and each secret is BLAKE2b(pool, fresh RNG output); the
// pool is then ratcheted, so the secret cannot be recomputed from it later.
//
// After a deep-sleep wake the AP is often not up yet when the first file
// opens, and a plain static pool started from zeros: those keys came from the
// RNG with no entropy source at all. So the pool is kept in RTC memory across
// sleep, and seeded at every boot from the RNG with the SAR ADC noise source
// switched on (logsink_seed) - in setup(), before the radio exists, because
// that source must not run alongside WiFi or the ADC.
static RTC_DATA_ATTR uint8_t entropy[32];
static uint32_t entropyStirs = 0;
static bool     entropySeeded = false;

void logsink_seed() {
  uint8_t m[32 + 32];
  memcpy(m, entropy, 32);
  bootloader_random_enable();
  esp_fill_random(m + 32, 32);
  bootloader_random_disable();
  crypto_blake2b(entropy, sizeof(entropy), m, sizeof(m));
  crypto_wipe(m, sizeof(m));
  entropySeeded = true;
}

void logsink_stir() {
  if (!web_is_up()) return;
  uint8_t m[32 + 16];
  memcpy(m, entropy, 32);
  esp_fill_random(m + 32, 16);
  crypto_blake2b(entropy, sizeof(entropy), m, sizeof(m));
  crypto_wipe(m, sizeof(m));
  entropyStirs++;
}

static void ephSecret(uint8_t out[32]) {
  uint8_t m[32 + 32 + 4];
  memcpy(m, entropy, 32);
  esp_fill_random(m + 32, 32);
  memcpy(m + 64, &entropyStirs, 4);
  crypto_blake2b(out, 32, m, sizeof(m));
  m[0] ^= 0x01;                                    // a different input for the ratchet
  crypto_blake2b(entropy, sizeof(entropy), m, sizeof(m));
  crypto_wipe(m, sizeof(m));
  if (!entropySeeded && entropyStirs == 0)
    Serial.println(F("[ERR] log key from an unseeded RNG - logsink_seed() never ran"));
}

// Where the logs' RAM is, and why (2026-10-17). The batch buffers (each
// LogFile's buf) are only ever filled and read by the CPU, so they are in
// PSRAM, allocated by logfile_alloc(). Everything that goes to the card
// passes through ioBuf, the one buffer in internal RAM: the SD driver DMAs
// only from internal memory, and from PSRAM it falls back to copying one
// 512-byte sector per command - each batch would turn into a string of single
// sector writes.
//
// Before, the batches and a 12 KB sealing buffer were static arrays, 32 KB of
// internal RAM held for good. That left the WiFi AP short of the
// AP_MIN_INTERNAL_HEAP it needs to come back after a drive: from log
// encryption (2026-10-12) on it never did, on any drive, until a reboot.
//
// Encrypted, each ioBuf's worth is one record (LOGCRYPT_OVERHEAD extra per
// 4 KB; a reader takes any record length). All writers run on the logger
// task or under the SD lock, so one buffer serves them all.
static constexpr size_t IO_CHUNK = 4096;
alignas(4) static uint8_t ioBuf[IO_CHUNK + LOGCRYPT_OVERHEAD];

// No fallback to internal RAM: that would quietly take back what the WiFi AP
// needs. Without PSRAM the board is faulty anyway.
bool logfile_alloc(LogFile &lf, size_t cap) {
  if (!lf.buf) lf.buf = (char *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
  lf.cap = lf.buf ? cap : 0;
  return lf.buf != nullptr;
}

// The write results used to be ignored: a card that failed mid-drive went on
// "logging" into nothing, with no fault on screen, and a broken encrypted
// block made the readers stop there and drop the rest of the file.
static volatile bool writeFailed = false;

void logfile_check_write(const LogFile &lf, size_t wrote, size_t want) {
  if (wrote == want) return;
  if (!writeFailed)
    Serial.printf("[ERR] SD write to %s came up short (%u of %u bytes)\n",
                  lf.path, (unsigned)wrote, (unsigned)want);
  writeFailed = true;
}

bool logsink_take_failure() {
  const bool f = writeFailed;
  writeFailed = false;
  return f;
}

void logfile_begin(LogFile &lf) {
  lf.len = 0;
  lf.enc = settings.logKey;
  if (!lf.enc) return;
  uint8_t eph[32], head[LOGCRYPT_HEADER_LEN];
  ephSecret(eph);
  logcrypt_begin(&lf.seg, settings.logPub, eph, head);   // wipes eph
  logfile_check_write(lf, lf.f.write(head, sizeof(head)), sizeof(head));
}

// IO_CHUNK at a time through ioBuf: sealed into it, or copied - the copy is
// what keeps each write one multi-sector DMA transfer.
void logfile_drain(LogFile &lf) {
  if (!lf.len) return;
  const uint8_t *p = (const uint8_t *)lf.buf;
  size_t n = lf.len;
  while (n) {
    const size_t k = n < IO_CHUNK ? n : IO_CHUNK;
    size_t len = k;
    if (lf.enc) len = logcrypt_seal(&lf.seg, p, k, ioBuf);
    else        memcpy(ioBuf, p, k);
    logfile_check_write(lf, lf.f.write(ioBuf, len), len);
    p += k;
    n -= k;
  }
  lf.len = 0;
  if (lf.afterDrain) lf.afterDrain(lf);
}

void logfile_commit(LogFile &lf) {
  logfile_drain(lf);
  lf.f.flush();
}

bool logfile_vprintf(LogFile &lf, const char *fmt, va_list ap) {
  for (int attempt = 0; attempt < 2; attempt++) {
    // Straight into the batch; on overflow vsnprintf leaves a cut-off copy
    // past len, which is ignored and overwritten by the retry.
    const size_t room = lf.cap - lf.len;
    va_list aq;
    va_copy(aq, ap);                    // a va_list is used up by one pass
    const int w = vsnprintf(lf.buf + lf.len, room, fmt, aq);
    va_end(aq);
    if (w < 0) return false;
    if ((size_t)w < room) { lf.len += (size_t)w; return true; }
    if (lf.len == 0) return false;      // longer than the whole batch
    logfile_drain(lf);
  }
  return false;
}

bool logfile_printf(LogFile &lf, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  const bool ok = logfile_vprintf(lf, fmt, ap);
  va_end(ap);
  return ok;
}

bool logfile_puts(LogFile &lf, const char *s) { return logfile_printf(lf, "%s", s); }

void logfile_end(LogFile &lf) {
  if (lf.enc) logcrypt_end(&lf.seg);
  lf.enc = false;
}
