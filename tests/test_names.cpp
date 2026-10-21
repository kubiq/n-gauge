// ============================================================================
// test_names.cpp  —  which card file names the web API accepts from the phone
// (calc_safe_name, calc_is_drive_log in calc.h). The security boundary of the
// file pages: a name that gets through is opened, served or deleted.
// ============================================================================
#include "test.h"
#include <string>
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
  char s[CALC_SAFE_NAME_MAX + 2];
  memset(s, 'a', sizeof(s) - 1);
  s[sizeof(s) - 1] = '\0';
  CHECK(!calc_safe_name(s, CALC_SAFE_NAME_MAX + 1));
  CHECK(calc_safe_name(s, CALC_SAFE_NAME_MAX));
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

// The firmware update's "is this ours" check.
static bool scan(const char *mark, const char *data, size_t chunk) {
  CalcMarkScan s;
  const size_t n = strlen(data);
  for (size_t i = 0; i < n; i += chunk)
    calc_mark_scan(&s, mark, (const uint8_t *)data + i, n - i < chunk ? n - i : chunk);
  return s.seen;
}

static void the_firmware_mark_is_found_however_the_image_is_chunked() {
  const char *img = "\xE9\x05junk N-Gaug N-Gauge fw tail";
  for (size_t chunk = 1; chunk <= strlen(img); chunk++)
    CHECK(scan("N-Gauge fw", img, chunk));
  CHECK(scan("N-Gauge fw", "NN-Gauge fw", 3));          // a restart on the first char
  CHECK(!scan("N-Gauge fw", "another sketch, N-Gauge f", 4));
  CHECK(!scan("N-Gauge fw", "", 4));
}

// Form values from a query string or a urlencoded body (the web server hands
// them over raw since esp_http_server).
static void form_values_are_found_and_decoded() {
  char v[32];
  const char *f = "a=1&apSsid=my+car%21&empty=&flag&tz=UTC%2B1&bad=%zz%4";
  CHECK(calc_form_value(f, "a", v, sizeof(v)) && strcmp(v, "1") == 0);
  CHECK(calc_form_value(f, "apSsid", v, sizeof(v)) && strcmp(v, "my car!") == 0);
  CHECK(calc_form_value(f, "empty", v, sizeof(v)) && v[0] == '\0');
  CHECK(calc_form_value(f, "flag", v, sizeof(v)) && v[0] == '\0');
  CHECK(calc_form_value(f, "tz", v, sizeof(v)) && strcmp(v, "UTC+1") == 0);
  CHECK(calc_form_value(f, "bad", v, sizeof(v)) && strcmp(v, "%zz%4") == 0);   // not escapes: as sent
  CHECK(!calc_form_value(f, "ap", v, sizeof(v)));          // a prefix is not the key
  CHECK(!calc_form_value(f, "missing", v, sizeof(v)) && v[0] == '\0');
  char small[4];
  CHECK(calc_form_value(f, "apSsid", small, sizeof(small)) && strcmp(small, "my ") == 0);
  CHECK(!calc_form_value("", "a", v, sizeof(v)));
}

static void multipart_boundary_from_the_header() {
  char b[72];
  CHECK(calc_multipart_boundary("multipart/form-data; boundary=----WebKitX1", b, sizeof(b)) &&
        strcmp(b, "----WebKitX1") == 0);
  CHECK(calc_multipart_boundary("multipart/form-data; boundary=\"a b\"; x=y", b, sizeof(b)) &&
        strcmp(b, "a b") == 0);
  CHECK(!calc_multipart_boundary("application/octet-stream", b, sizeof(b)));
  CHECK(!calc_multipart_boundary("multipart/form-data", b, sizeof(b)));
}

// The content of the first part, whatever the chunking - including content
// that holds a CR LF, "--", and the start of the boundary.
static std::string multipart_read(const std::string &body, const char *boundary, size_t chunk,
                                  bool *ended) {
  CalcMultipart m;
  CHECK(calc_multipart_begin(&m, boundary));
  std::string out;
  bool done = false;
  for (size_t i = 0; i < body.size() && !done; i += chunk) {
    const size_t n = body.size() - i < chunk ? body.size() - i : chunk;
    done = calc_multipart_feed(&m, (const uint8_t *)body.data() + i, n,
                               [&](const uint8_t *d, size_t k) { out.append((const char *)d, k); });
  }
  *ended = done;
  return out;
}

static void multipart_content_streams_out_in_any_chunks() {
  const char *B = "XyZ123";
  const std::string content = std::string("\xE9\x00\x01 a\r\nb\r\n--Xy\r\n--XyZ12 end\r", 32);
  const std::string body = std::string("--") + B + "\r\n"
      "Content-Disposition: form-data; name=\"fw\"; filename=\"fw.bin\"\r\n"
      "Content-Type: application/octet-stream\r\n\r\n" +
      content + "\r\n--" + B + "--\r\n";
  for (size_t chunk = 1; chunk <= body.size(); chunk++) {
    bool ended = false;
    const std::string got = multipart_read(body, B, chunk, &ended);
    CHECK(got == content);
    CHECK(ended);
  }
  // A body cut before its closing boundary: what came, and not ended.
  bool ended = true;
  const std::string cut = body.substr(0, body.size() - 20);
  CHECK(multipart_read(cut, B, 7, &ended).size() <= content.size());
  CHECK(!ended);
}

int main() {
  printf("test_names\n");
  TEST(form_values_are_found_and_decoded);
  TEST(multipart_boundary_from_the_header);
  TEST(multipart_content_streams_out_in_any_chunks);
  TEST(the_firmware_mark_is_found_however_the_image_is_chunked);
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
