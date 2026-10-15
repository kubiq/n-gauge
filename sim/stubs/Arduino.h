// Stand-in for the Arduino core, for the gauge firmware on the PC (sim/).
// millis() is the simulation's clock - wall time times the replay speed,
// paused or sought by the keys in the window (sim_main.cpp) - so the
// firmware's timeouts, LVGL's animations and the replayed drive agree.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#define RTC_DATA_ATTR
#define IRAM_ATTR
#define F(s) (s)
#define HIGH 1
#define LOW 0
#define OUTPUT 1
#define INPUT 0
#define INPUT_PULLUP 2

uint32_t millis();
uint32_t micros();
void delay(uint32_t ms);
inline void yield() {}
inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
inline int digitalRead(int) { return HIGH; }

// Enough of Arduino's String for the serial console's commands.
class String {
 public:
  String() = default;
  String(const char *s) : s_(s ? s : "") {}
  String(const std::string &s) : s_(s) {}
  const char *c_str() const { return s_.c_str(); }
  size_t length() const { return s_.size(); }
  void trim() {
    const auto a = s_.find_first_not_of(" \t\r\n");
    const auto b = s_.find_last_not_of(" \t\r\n");
    s_ = a == std::string::npos ? "" : s_.substr(a, b - a + 1);
  }
  bool startsWith(const char *p) const { return s_.rfind(p, 0) == 0; }
  String substring(size_t from) const { return from < s_.size() ? String(s_.substr(from)) : String(); }
  int toInt() const { return atoi(s_.c_str()); }
  bool operator==(const char *o) const { return s_ == o; }

 private:
  std::string s_;
};

// The serial console: output to the terminal; no input (the window's keys
// drive the simulation instead).
struct SimSerial {
  void begin(long) {}
  template <typename... A> void printf(const char *fmt, A... a) { std::printf(fmt, a...); std::fflush(stdout); }
  void println() { std::printf("\n"); }
  void println(const char *s) { std::printf("%s\n", s); }
  void println(const String &s) { std::printf("%s\n", s.c_str()); }
  template <typename T> void println(T v) { std::printf("%ld\n", (long)v); }
  void print(const char *s) { std::printf("%s", s); }
  template <typename T> void print(T v) { std::printf("%ld", (long)v); }
  int available() { return 0; }
  int read() { return -1; }
  long parseInt() { return 0; }
  String readStringUntil(char) { return String(); }
  explicit operator bool() const { return true; }
};
extern SimSerial Serial;

struct SimEsp {
  uint32_t getFreeHeap() { return 180 * 1024; }
  uint32_t getFreePsram() { return 7 * 1024 * 1024; }
  uint32_t getSketchSize() { return 2 * 1024 * 1024; }
  void restart() { std::exit(0); }
};
extern SimEsp ESP;

inline float temperatureRead() { return 42.0f; }

enum esp_reset_reason_t { ESP_RST_UNKNOWN, ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_SW, ESP_RST_PANIC,
                          ESP_RST_INT_WDT, ESP_RST_TASK_WDT, ESP_RST_WDT, ESP_RST_DEEPSLEEP, ESP_RST_BROWNOUT };
inline esp_reset_reason_t esp_reset_reason() { return ESP_RST_POWERON; }
