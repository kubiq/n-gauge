// ============================================================================
// test_logcrypt.cpp  —  the encrypted log format (logcrypt.h).
//
// Round trips, the failure cases a car produces (a power cut mid-record, a
// reopened file), and a fixed test vector: tests/vectors/logcrypt-v1.json.
// The vector is what ties the pod to its readers - the app decrypts the same
// bytes with @noble/* in its own tests - so this test also fails if the
// format drifts from it. To regenerate after a deliberate format change (and
// a version bump): LOGCRYPT_WRITE_VECTOR=1 ./scripts/test.sh
// ============================================================================
#include "test.h"
#include <cstdlib>
#include <string>
#include "../firmware/car_gauge_v1/logcrypt.h"

static const char *VECTOR = "tests/vectors/logcrypt-v1.json";

// Fixed secrets: a test vector, not keys anyone uses.
static void fill(uint8_t *p, size_t n, uint8_t start) {
  for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(start + i);
}

static void recipient(uint8_t sec[32], uint8_t pub[32]) {
  fill(sec, 32, 0x40);
  crypto_x25519_public_key(pub, sec);
}

// A file of `segs` segments with these records each - as the pod writes it.
static std::string write_file(const uint8_t pub[32], const char *const *recs, int nrec, int segs) {
  std::string f;
  for (int s = 0; s < segs; s++) {
    LogcryptSeg seg;
    uint8_t eph[32], head[LOGCRYPT_HEADER_LEN];
    fill(eph, 32, (uint8_t)(0x10 + 0x20 * s));
    logcrypt_begin(&seg, pub, eph, head);
    f.append((const char *)head, sizeof(head));
    for (int r = 0; r < nrec; r++) {
      const size_t n = strlen(recs[r]);
      uint8_t out[256];
      logcrypt_seal(&seg, (const uint8_t *)recs[r], n, out);
      f.append((const char *)out, n + LOGCRYPT_OVERHEAD);
    }
    logcrypt_end(&seg);
  }
  return f;
}

static std::string read_back(const std::string &f, const uint8_t sec[32], size_t *consumed) {
  uint8_t out[4096];
  const long n = logcrypt_open((const uint8_t *)f.data(), f.size(), sec, out, sizeof(out), consumed);
  return n < 0 ? std::string("<overflow>") : std::string((const char *)out, (size_t)n);
}

static const char *const ROWS[] = {"utc,ms,oilC\n", ",1000,85\n", ",1500,86\n"};

static void round_trip() {
  uint8_t sec[32], pub[32];
  recipient(sec, pub);
  const std::string f = write_file(pub, ROWS, 3, 1);
  CHECK(f.size() == LOGCRYPT_HEADER_LEN + 3 * LOGCRYPT_OVERHEAD + strlen(ROWS[0]) + strlen(ROWS[1]) + strlen(ROWS[2]));
  size_t used = 0;
  CHECK(read_back(f, sec, &used) == "utc,ms,oilC\n,1000,85\n,1500,86\n");
  CHECK(used == f.size());
}

static void the_secret_is_wiped_and_the_plaintext_is_not_in_the_file() {
  uint8_t sec[32], pub[32], eph[32], head[LOGCRYPT_HEADER_LEN];
  recipient(sec, pub);
  fill(eph, 32, 0x10);
  LogcryptSeg seg;
  logcrypt_begin(&seg, pub, eph, head);
  uint8_t zero[32] = {0};
  CHECK(memcmp(eph, zero, 32) == 0);
  uint8_t out[64];
  logcrypt_seal(&seg, (const uint8_t *)"50.0568,14.4705", 15, out);
  CHECK(std::string((const char *)out, 15 + LOGCRYPT_OVERHEAD).find("50.05") == std::string::npos);
  logcrypt_end(&seg);
}

// A wake or a boot reopens the file: a second header, a fresh key.
static void two_segments_read_in_order() {
  uint8_t sec[32], pub[32];
  recipient(sec, pub);
  const std::string f = write_file(pub, ROWS + 1, 2, 2);
  CHECK(read_back(f, sec, nullptr) == ",1000,85\n,1500,86\n,1000,85\n,1500,86\n");
}

// The ignition cuts power mid-write: the last record is short.
static void a_cut_off_record_ends_the_file_cleanly() {
  uint8_t sec[32], pub[32];
  recipient(sec, pub);
  const std::string f = write_file(pub, ROWS, 3, 1);
  const size_t last = f.size() - (strlen(ROWS[2]) + LOGCRYPT_OVERHEAD);
  for (size_t cut = last; cut < f.size(); cut++) {
    size_t used = 0;
    CHECK(read_back(f.substr(0, cut), sec, &used) == "utc,ms,oilC\n,1000,85\n");
    CHECK(used == last);
  }
}

static void a_changed_byte_stops_the_reader_there() {
  uint8_t sec[32], pub[32];
  recipient(sec, pub);
  std::string f = write_file(pub, ROWS, 3, 1);
  const size_t second = LOGCRYPT_HEADER_LEN + strlen(ROWS[0]) + LOGCRYPT_OVERHEAD;
  f[second + LOGCRYPT_BLOCK_HEAD + 2] ^= 1;   // inside record 2's ciphertext
  size_t used = 0;
  CHECK(read_back(f, sec, &used) == "utc,ms,oilC\n");
  CHECK(used == second);
}

// A write that failed mid-block on a card error, then the pod carried on in a
// new segment: the reader skips the broken block and reads the rest.
static void a_broken_block_is_skipped_to_the_next_segment() {
  uint8_t sec[32], pub[32];
  recipient(sec, pub);
  const std::string seg = write_file(pub, ROWS + 1, 2, 1);
  const std::string broken = seg.substr(0, seg.size() - 7);   // last record cut
  const std::string f = broken + seg + std::string("\x02\x10\x00\x00\x00junk", 9) + seg;
  size_t used = 0;
  CHECK(read_back(f, sec, &used) == ",1000,85\n,1000,85\n,1500,86\n,1000,85\n,1500,86\n");
  CHECK(used == f.size());
}

static void the_wrong_key_reads_nothing() {
  uint8_t sec[32], pub[32], other[32];
  recipient(sec, pub);
  fill(other, 32, 0x41);
  CHECK(read_back(write_file(pub, ROWS, 3, 1), other, nullptr).empty());
}

static void a_plaintext_file_is_not_mistaken_for_one() {
  uint8_t sec[32], pub[32];
  recipient(sec, pub);
  size_t used = 1;
  CHECK(read_back("utc,ms,oilC\n,1000,85\n", sec, &used).empty());
  CHECK(used == 0);
}

// A key that would make every file's key public is refused.
static void low_order_keys_are_refused() {
  uint8_t sec[32], pub[32];
  recipient(sec, pub);
  CHECK(logcrypt_pub_ok(pub));
  uint8_t bad[32] = { 0 };
  CHECK(!logcrypt_pub_ok(bad));                              // 0
  bad[0] = 1;
  CHECK(!logcrypt_pub_ok(bad));                              // 1
  static const uint8_t order8[32] = {
    0xe0, 0xeb, 0x7a, 0x7c, 0x3b, 0x41, 0xb8, 0xae, 0x16, 0x56, 0xe3, 0xfa, 0xf1, 0x9f, 0xc4, 0x6a,
    0xda, 0x09, 0x8d, 0xeb, 0x9c, 0x32, 0xb1, 0xfd, 0x86, 0x62, 0x05, 0x16, 0x5f, 0x49, 0xb8, 0x00 };
  CHECK(!logcrypt_pub_ok(order8));
  memset(bad, 0xff, 32);
  bad[0] = 0xec; bad[31] = 0x7f;                             // p - 1
  CHECK(!logcrypt_pub_ok(bad));
}

static void fingerprint_is_12_hex_digits() {
  uint8_t sec[32], pub[32];
  recipient(sec, pub);
  char fp[13];
  logcrypt_fingerprint(fp, pub);
  CHECK(strlen(fp) == 12);
  CHECK(strspn(fp, "0123456789abcdef") == 12);
}

static std::string hex(const uint8_t *p, size_t n) {
  static const char h[] = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; i++) { s += h[p[i] >> 4]; s += h[p[i] & 15]; }
  return s;
}

// Everything the readers need to reproduce the file byte for byte.
static std::string vector_json() {
  uint8_t sec[32], pub[32], eph0[32], eph1[32];
  recipient(sec, pub);
  fill(eph0, 32, 0x10);
  fill(eph1, 32, 0x30);
  char fp[13];
  logcrypt_fingerprint(fp, pub);
  const std::string f = write_file(pub, ROWS, 3, 2);
  std::string j = "{\n";
  j += "  \"note\": \"logcrypt v1 test vector, written by tests/test_logcrypt.cpp. Fixed secrets - not keys anyone uses.\",\n";
  j += "  \"recipientSecret\": \"" + hex(sec, 32) + "\",\n";
  j += "  \"recipientPublic\": \"" + hex(pub, 32) + "\",\n";
  j += "  \"fingerprint\": \"" + std::string(fp) + "\",\n";
  j += "  \"ephemeralSecrets\": [\"" + hex(eph0, 32) + "\", \"" + hex(eph1, 32) + "\"],\n";
  j += "  \"records\": [\"utc,ms,oilC\\n\", \",1000,85\\n\", \",1500,86\\n\"],\n";
  j += "  \"plaintext\": \"utc,ms,oilC\\n,1000,85\\n,1500,86\\nutc,ms,oilC\\n,1000,85\\n,1500,86\\n\",\n";
  j += "  \"file\": \"" + hex((const uint8_t *)f.data(), f.size()) + "\"\n";
  j += "}\n";
  return j;
}

static void matches_the_committed_vector() {
  const std::string want = vector_json();
  if (getenv("LOGCRYPT_WRITE_VECTOR")) {
    FILE *o = fopen(VECTOR, "w");
    CHECK(o != nullptr);
    if (o) { fputs(want.c_str(), o); fclose(o); printf("  wrote %s\n", VECTOR); }
    return;
  }
  FILE *i = fopen(VECTOR, "r");
  CHECK(i != nullptr);
  if (!i) return;
  std::string have;
  char buf[4096];
  for (size_t n; (n = fread(buf, 1, sizeof(buf), i)) > 0;) have.append(buf, n);
  fclose(i);
  CHECK(have == want);
}

int main() {
  printf("test_logcrypt\n");
  TEST(round_trip);
  TEST(the_secret_is_wiped_and_the_plaintext_is_not_in_the_file);
  TEST(two_segments_read_in_order);
  TEST(a_cut_off_record_ends_the_file_cleanly);
  TEST(a_changed_byte_stops_the_reader_there);
  TEST(a_broken_block_is_skipped_to_the_next_segment);
  TEST(the_wrong_key_reads_nothing);
  TEST(a_plaintext_file_is_not_mistaken_for_one);
  TEST(low_order_keys_are_refused);
  TEST(fingerprint_is_12_hex_digits);
  TEST(matches_the_committed_vector);
  TEST_SUMMARY();
}
