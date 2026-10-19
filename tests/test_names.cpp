// ============================================================================
// test_names.cpp  —  which card file names the web API accepts from the phone
// (calc_safe_name, calc_is_drive_log in calc.h). The security boundary of the
// file pages: a name that gets through is opened, served or deleted.
// ============================================================================
#include "test.h"
#include "../firmware/car_gauge_v1/calc.h"
#include "../firmware/car_gauge_v1/logcrypt.h"   // LOGCRYPT_SUFFIX, as the firmware uses it

static bool safe(const char *s) { return calc_safe_name(s, strlen(s)); }

static void every_name_the_firmware_writes_is_safe() {
  CHECK(safe("drive_2026-09-25_14-32_3fa81c.csv"));
  CHECK(safe("trip_2026-09-25_14-32_3fa81c.gpx.nge"));
  CHECK(safe("track_0012_3fa81c.vbo"));
  CHECK(safe("sleeplog.txt"));
  // The longest a dated name gets: a second file in the same minute, encrypted.
  CHECK(safe("drive_2026-09-25_14-32_9_3fa81c.csv.nge"));
}

static void separators_and_parent_dirs_are_refused() {
  CHECK(!safe("../secret"));
  CHECK(!safe("a/b"));
  CHECK(!safe("a\\b"));
  CHECK(!safe("x..y"));
  CHECK(!safe(".."));
  CHECK(!safe("/sleeplog.txt"));
}

// The bug the whitelist was written against: a NUL inside the name, with the
// length still counting what follows it.
static void an_embedded_nul_is_refused() {
  const char s[] = "x\0/../secret";
  CHECK(!calc_safe_name(s, sizeof(s) - 1));
}

static void header_breaking_and_odd_characters_are_refused() {
  CHECK(!safe("a\r\nSet-Cookie:x"));
  CHECK(!safe("a b.csv"));
  CHECK(!safe("a\"b"));
  CHECK(!safe("a%2fb"));
}

static void empty_and_overlong_are_refused() {
  CHECK(!safe(""));
  CHECK(!calc_safe_name(nullptr, 3));
  CHECK(safe("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));     // 40
  CHECK(!safe("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));   // 41
}

static bool log(const char *s) { return calc_is_drive_log(s, LOGCRYPT_SUFFIX); }

static void drive_logs_are_deletable() {
  CHECK(log("drive_2026-09-25_14-32_3fa81c.csv"));
  CHECK(log("trip_0012_3fa81c.gpx"));
  CHECK(log("track_2026-10-17_13-42_08cc3a.vbo"));
  CHECK(log("drive_2026-09-25_14-32_3fa81c.csv.nge"));
  CHECK(log("trip_0012_3fa81c.gpx.nge"));
}

static void nothing_else_is_deletable() {
  CHECK(!log("sleeplog.txt"));
  CHECK(!log("firmware.bin"));
  CHECK(!log("settings.json"));
  CHECK(!log("drive_x.bin"));
  CHECK(!log("notes_drive_x.csv"));        // the kind must be the prefix
  CHECK(!log("drive_x.csv.nge.nge"));      // the suffix comes off once
  CHECK(!log("trip_.nge"));
  CHECK(!log("trip_"));
  CHECK(!log(nullptr));
}

// ?p= on the card listings. The bug: toInt() made a huge p INT_MAX, and
// p * perPage overflowed to a negative index into the name table.
static void page_numbers_are_digits_in_range() {
  CHECK_EQ(calc_parse_page("0", 100000), 0);
  CHECK_EQ(calc_parse_page("12", 100000), 12);
  CHECK_EQ(calc_parse_page("100000", 100000), 100000);
  CHECK_EQ(calc_parse_page("100001", 100000), -1);
  CHECK_EQ(calc_parse_page("99999999999999999999", 100000), -1);
  CHECK_EQ(calc_parse_page("-1", 100000), -1);
  CHECK_EQ(calc_parse_page("1a", 100000), -1);
  CHECK_EQ(calc_parse_page(" 1", 100000), -1);
  CHECK_EQ(calc_parse_page("", 100000), -1);
  CHECK_EQ(calc_parse_page(nullptr, 100000), -1);
  // The largest page times the largest page size still fits an int.
  CHECK(100000LL * 50 < 2147483647LL);
}

int main() {
  printf("test_names\n");
  TEST(page_numbers_are_digits_in_range);
  TEST(every_name_the_firmware_writes_is_safe);
  TEST(separators_and_parent_dirs_are_refused);
  TEST(an_embedded_nul_is_refused);
  TEST(header_breaking_and_odd_characters_are_refused);
  TEST(empty_and_overlong_are_refused);
  TEST(drive_logs_are_deletable);
  TEST(nothing_else_is_deletable);
  TEST_SUMMARY();
}
