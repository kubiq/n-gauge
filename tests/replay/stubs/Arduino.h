// Stand-in for the Arduino core, so firmware files that need only its clock
// and serial port compile on the PC (tests/replay/). millis() is the
// replay's clock, set by the harness from the recorded fixes.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#define RTC_DATA_ATTR
#define F(s) (s)

extern uint32_t replayMillis;
inline uint32_t millis() { return replayMillis; }

struct ReplaySerial {
  template <typename... A> void printf(const char *fmt, A... a) { std::printf(fmt, a...); }
  void println(const char *s) { std::printf("%s\n", s); }
  void print(const char *s) { std::printf("%s", s); }
};
extern ReplaySerial Serial;
