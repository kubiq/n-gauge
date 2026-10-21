// ============================================================================
// logcrypt.h  —  the encrypted log format: write side, and a reader for tests.
//
// Why and what for: DECISIONS.md, "Logs are to be encrypted to a public key".
// In short: the phone holds an X25519 private key, the pod only its public
// key. Each segment of a log file gets a throwaway key pair; the pod derives
// the segment key from it and the phone's public key, writes the throwaway
// public half in a header, and forgets the secret half - after which not even
// the pod can read what it wrote.
//
// Pure: no Arduino, no RNG, no files - bytes in, bytes out - so the same code
// runs in tests/ on the host. The caller supplies the throwaway secret (from
// the hardware RNG on the pod) and writes the bytes produced.
//
// ---- On the card -----------------------------------------------------------
// A file is a sequence of blocks:  type (1) | length (u32 LE) | body.
//   0x01 header  body = "NGE1" | version 0x01 | eph_pub (32)        37 bytes
//   0x02 record  body = ciphertext | mac (16)
// A segment is a header followed by its records; reopening a file (a boot, a
// wake) starts a new segment. Each record is one flushed batch of plaintext:
//   key   = BLAKE2b-256(X25519(eph, recipient) | eph_pub | recipient_pub
//                       | "n-gauge log v1")
//   nonce = the record's index in its segment, u64 LE, then 16 zero bytes
//   AEAD  = XChaCha20-Poly1305 (Monocypher's crypto_aead_lock), the header
//           body as associated data
// Files stay append-only: the sync's {name, size} and Range resume work as
// before. A record cut short by a power loss fails its MAC; the reader stops
// there with everything before it intact.
//
// Monocypher (monocypher.c/.h, 4.0.2, BSD-2-Clause or CC0, see
// monocypher-LICENCE.md) rather than the core's mbedTLS: that build has no
// ChaCha20-Poly1305, and one portable file is testable on the host too.
// ============================================================================
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "monocypher.h"

#define LOGCRYPT_TYPE_HEADER  0x01
#define LOGCRYPT_TYPE_RECORD  0x02
#define LOGCRYPT_VERSION      0x01
#define LOGCRYPT_KEY_LEN      32
#define LOGCRYPT_MAC_LEN      16
#define LOGCRYPT_BLOCK_HEAD   5                       // type + u32 length
#define LOGCRYPT_HEADER_BODY  (4 + 1 + LOGCRYPT_KEY_LEN)
#define LOGCRYPT_HEADER_LEN   (LOGCRYPT_BLOCK_HEAD + LOGCRYPT_HEADER_BODY)
#define LOGCRYPT_OVERHEAD     (LOGCRYPT_BLOCK_HEAD + LOGCRYPT_MAC_LEN)   // per record
// A reader refuses records longer than this rather than trusting a garbled
// length. The pod's batches are a few KB (the GPX buffer is 12 KB).
#define LOGCRYPT_MAX_RECORD   (256u * 1024u)

// An encrypted file's name is the plain name plus this: drive_<stamp>.csv.nge,
// trip_<stamp>.gpx.nge, track_<stamp>.vbo.nge. Readers decide by the name,
// and decrypting gives back the plain name; nobody opens ciphertext in a
// spreadsheet thinking the log is corrupt.
#define LOGCRYPT_SUFFIX       ".nge"

static const uint8_t LOGCRYPT_MAGIC[4] = {'N', 'G', 'E', '1'};
static const char    LOGCRYPT_INFO[]   = "n-gauge log v1";

struct LogcryptSeg {
  uint8_t  key[LOGCRYPT_KEY_LEN];
  uint8_t  ad[LOGCRYPT_HEADER_BODY];   // the header body
  uint64_t counter;
  bool     active;
};

static inline void logcrypt_put_u32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline uint32_t logcrypt_get_u32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void logcrypt_nonce(uint8_t nonce[24], uint64_t counter) {
  memset(nonce, 0, 24);
  for (int i = 0; i < 8; i++) nonce[i] = (uint8_t)(counter >> (8 * i));
}

// key = BLAKE2b-256(shared | eph_pub | recipient_pub | info)
static inline void logcrypt_derive(uint8_t key[LOGCRYPT_KEY_LEN], const uint8_t shared[32],
                                   const uint8_t eph_pub[32], const uint8_t recipient_pub[32]) {
  uint8_t msg[32 + 32 + 32 + sizeof(LOGCRYPT_INFO) - 1];
  memcpy(msg, shared, 32);
  memcpy(msg + 32, eph_pub, 32);
  memcpy(msg + 64, recipient_pub, 32);
  memcpy(msg + 96, LOGCRYPT_INFO, sizeof(LOGCRYPT_INFO) - 1);
  crypto_blake2b(key, LOGCRYPT_KEY_LEN, msg, sizeof(msg));
  crypto_wipe(msg, sizeof(msg));
}

// Starts a segment: derives its key, writes its header block to out
// (LOGCRYPT_HEADER_LEN bytes), and WIPES eph_secret - the point of the
// design is that it exists only for the length of this call.
static inline size_t logcrypt_begin(LogcryptSeg *s, const uint8_t recipient_pub[32],
                                    uint8_t eph_secret[32], uint8_t out[LOGCRYPT_HEADER_LEN]) {
  uint8_t eph_pub[32], shared[32];
  crypto_x25519_public_key(eph_pub, eph_secret);
  crypto_x25519(shared, eph_secret, recipient_pub);
  crypto_wipe(eph_secret, 32);
  logcrypt_derive(s->key, shared, eph_pub, recipient_pub);
  crypto_wipe(shared, sizeof(shared));

  memcpy(s->ad, LOGCRYPT_MAGIC, 4);
  s->ad[4] = LOGCRYPT_VERSION;
  memcpy(s->ad + 5, eph_pub, 32);
  s->counter = 0;
  s->active = true;

  out[0] = LOGCRYPT_TYPE_HEADER;
  logcrypt_put_u32(out + 1, LOGCRYPT_HEADER_BODY);
  memcpy(out + LOGCRYPT_BLOCK_HEAD, s->ad, LOGCRYPT_HEADER_BODY);
  return LOGCRYPT_HEADER_LEN;
}

// One record: n bytes of plaintext in, n + LOGCRYPT_OVERHEAD bytes out.
// out must not overlap plain.
static inline size_t logcrypt_seal(LogcryptSeg *s, const uint8_t *plain, size_t n, uint8_t *out) {
  uint8_t nonce[24];
  logcrypt_nonce(nonce, s->counter++);
  out[0] = LOGCRYPT_TYPE_RECORD;
  logcrypt_put_u32(out + 1, (uint32_t)(n + LOGCRYPT_MAC_LEN));
  crypto_aead_lock(out + LOGCRYPT_BLOCK_HEAD, out + LOGCRYPT_BLOCK_HEAD + n, s->key, nonce,
                   s->ad, sizeof(s->ad), plain, n);
  return n + LOGCRYPT_OVERHEAD;
}

static inline void logcrypt_end(LogcryptSeg *s) {
  crypto_wipe(s, sizeof(*s));
}

// Whether `pub` can be encrypted to. X25519 against a low-order point - all
// zeros among them - gives an all-zero shared secret whatever our ephemeral
// key is, so every file would have a key anyone can derive: "encrypted" logs
// that are not. Clamping clears the cofactor, so one trial with any secret
// tells: such a point gives zero for every secret, a real key for none.
static inline bool logcrypt_pub_ok(const uint8_t pub[32]) {
  uint8_t trial[32], shared[32];
  memset(trial, 0x5A, sizeof(trial));
  crypto_x25519(shared, trial, pub);
  uint8_t any = 0;
  for (int i = 0; i < 32; i++) any |= shared[i];
  crypto_wipe(shared, sizeof(shared));
  return any != 0;
}

// Fingerprint of a public key, to compare the pod's against the phone's by
// eye: the first 6 bytes of BLAKE2b-256(pub), as 12 hex digits.
static inline void logcrypt_fingerprint(char out[13], const uint8_t pub[32]) {
  uint8_t h[32];
  crypto_blake2b(h, sizeof(h), pub, 32);
  static const char hex[] = "0123456789abcdef";
  for (int i = 0; i < 6; i++) { out[2 * i] = hex[h[i] >> 4]; out[2 * i + 1] = hex[h[i] & 15]; }
  out[12] = '\0';
}

// ---- reader (tests, and the reference for the app and the PC tool) --------
// Walks a whole file, decrypting record by record into out (out_cap bytes).
// Returns the plaintext length; *consumed says how far the file was valid -
// less than its size when it ends in a cut-off or corrupt block, which the
// reader treats as the end. -1 if out is too small.
static inline long logcrypt_open(const uint8_t *file, size_t n, const uint8_t recipient_secret[32],
                                 uint8_t *out, size_t out_cap, size_t *consumed) {
  uint8_t recipient_pub[32];
  crypto_x25519_public_key(recipient_pub, recipient_secret);
  LogcryptSeg s;
  memset(&s, 0, sizeof(s));
  size_t at = 0, len = 0;
  while (at + LOGCRYPT_BLOCK_HEAD <= n) {
    const uint8_t type = file[at];
    const uint32_t body = logcrypt_get_u32(file + at + 1);
    if (body > LOGCRYPT_MAX_RECORD || at + LOGCRYPT_BLOCK_HEAD + body > n) break;
    const uint8_t *b = file + at + LOGCRYPT_BLOCK_HEAD;
    if (type == LOGCRYPT_TYPE_HEADER) {
      if (body != LOGCRYPT_HEADER_BODY || memcmp(b, LOGCRYPT_MAGIC, 4) != 0 || b[4] != LOGCRYPT_VERSION) break;
      uint8_t shared[32];
      crypto_x25519(shared, recipient_secret, b + 5);
      logcrypt_derive(s.key, shared, b + 5, recipient_pub);
      crypto_wipe(shared, sizeof(shared));
      memcpy(s.ad, b, LOGCRYPT_HEADER_BODY);
      s.counter = 0;
      s.active = true;
    } else if (type == LOGCRYPT_TYPE_RECORD && s.active && body >= LOGCRYPT_MAC_LEN) {
      const size_t m = body - LOGCRYPT_MAC_LEN;
      if (len + m > out_cap) { crypto_wipe(&s, sizeof(s)); return -1; }
      uint8_t nonce[24];
      logcrypt_nonce(nonce, s.counter);
      if (crypto_aead_unlock(out + len, b + m, s.key, nonce, s.ad, sizeof(s.ad), b, m) != 0) break;
      s.counter++;
      len += m;
    } else {
      break;
    }
    at += LOGCRYPT_BLOCK_HEAD + body;
  }
  crypto_wipe(&s, sizeof(s));
  if (consumed) *consumed = at;
  return (long)len;
}
