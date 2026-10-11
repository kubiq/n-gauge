// ============================================================================
// test.h  —  minimal assertion harness for host-side tests.
//
// Deliberately tiny and dependency-free. The firmware is built by arduino-cli
// for an ESP32 target, so tests cannot link against it; instead each test file
// compiles the pure logic it needs (either by #including config.h, or by
// including a .cpp with the Arduino symbols stubbed) with a normal host
// compiler. Adding a framework would mean managing a toolchain for a few
// hundred lines of assertions.
//
// Usage: define tests as functions, call them from main(), end with
// TEST_SUMMARY(). Non-zero exit status on any failure, so it works in CI.
// ============================================================================
#pragma once
#include <cmath>
#include <cstdio>
#include <cstring>

static int tests_run = 0;
static int tests_failed = 0;
static const char *current_test = "";

#define TEST(name) \
  do { current_test = #name; tests_run++; name(); } while (0)

#define CHECK(cond)                                                      \
  do {                                                                   \
    if (!(cond)) {                                                       \
      tests_failed++;                                                    \
      printf("  FAIL %s:%d  %s\n      condition: %s\n",                  \
             __FILE__, __LINE__, current_test, #cond);                   \
    }                                                                    \
  } while (0)

#define CHECK_EQ(got, want)                                              \
  do {                                                                   \
    const long long g_ = (long long)(got), w_ = (long long)(want);       \
    if (g_ != w_) {                                                      \
      tests_failed++;                                                    \
      printf("  FAIL %s:%d  %s\n      %s = %lld, expected %lld\n",       \
             __FILE__, __LINE__, current_test, #got, g_, w_);            \
    }                                                                    \
  } while (0)

// Floats get an explicit tolerance - exact comparison of derived physical
// values is a false-failure generator.
#define CHECK_NEAR(got, want, tol)                                       \
  do {                                                                   \
    const double g_ = (double)(got), w_ = (double)(want);                \
    if (!(std::fabs(g_ - w_) <= (tol))) {                                \
      tests_failed++;                                                    \
      printf("  FAIL %s:%d  %s\n      %s = %.6f, expected %.6f +/-%.6f\n",\
             __FILE__, __LINE__, current_test, #got, g_, w_, (double)(tol)); \
    }                                                                    \
  } while (0)

#define CHECK_STREQ(got, want)                                           \
  do {                                                                   \
    if (strcmp((got), (want)) != 0) {                                    \
      tests_failed++;                                                    \
      printf("  FAIL %s:%d  %s\n      \"%s\" != \"%s\"\n",               \
             __FILE__, __LINE__, current_test, (got), (want));           \
    }                                                                    \
  } while (0)

#define TEST_SUMMARY()                                                   \
  do {                                                                   \
    printf("%s: %d test(s), %d failure(s)\n",                            \
           tests_failed ? "FAILED" : "ok", tests_run, tests_failed);     \
    return tests_failed ? 1 : 0;                                         \
  } while (0)
