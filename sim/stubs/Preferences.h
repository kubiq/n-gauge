// Stand-in for the ESP32's NVS (Preferences), in memory: the gauge's
// settings start from config.h's defaults on every run of the simulation,
// as on a freshly erased gauge.
#pragma once
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "Arduino.h"   // as the real one: String, Serial come with it

class Preferences {
 public:
  bool begin(const char *ns, bool = false) { ns_ = ns; return true; }
  void end() {}
  bool clear() { store()[ns_].clear(); return true; }
  bool remove(const char *k) { return store()[ns_].erase(k) > 0; }
  bool isKey(const char *k) { return store()[ns_].count(k) > 0; }

  size_t putBytes(const char *k, const void *v, size_t n) {
    auto &b = store()[ns_][k];
    b.assign((const uint8_t *)v, (const uint8_t *)v + n);
    return n;
  }
  size_t getBytes(const char *k, void *out, size_t n) {
    auto &ns = store()[ns_];
    auto it = ns.find(k);
    if (it == ns.end()) return 0;
    const size_t m = it->second.size() < n ? it->second.size() : n;
    std::memcpy(out, it->second.data(), m);
    return m;
  }
  size_t getBytesLength(const char *k) {
    auto &ns = store()[ns_];
    auto it = ns.find(k);
    return it == ns.end() ? 0 : it->second.size();
  }
#define SIM_PREF(T, Name)                                                         \
  size_t put##Name(const char *k, T v) { return putBytes(k, &v, sizeof(v)); }       \
  T get##Name(const char *k, T d = T()) { T v = d; getBytes(k, &v, sizeof(v)); return v; }
  SIM_PREF(uint8_t, UChar)
  SIM_PREF(int8_t, Char)
  SIM_PREF(uint16_t, UShort)
  SIM_PREF(int16_t, Short)
  SIM_PREF(uint32_t, UInt)
  SIM_PREF(int32_t, Int)
  SIM_PREF(float, Float)
  SIM_PREF(bool, Bool)
#undef SIM_PREF
  size_t putString(const char *k, const char *v) { return putBytes(k, v, std::strlen(v) + 1); }
  size_t putString(const char *k, const String &v) { return putString(k, v.c_str()); }
  String getString(const char *k, const char *def = "") {
    const size_t n = getBytesLength(k);
    if (!n) return String(def);
    std::string s(n, '\0');
    getBytes(k, &s[0], n);
    return String(std::string(s.c_str()));
  }
  size_t getString(const char *k, char *out, size_t n) {
    if (!n) return 0;
    out[0] = '\0';
    const size_t m = getBytes(k, out, n);
    out[n - 1] = '\0';
    return m;
  }

 private:
  using Ns = std::map<std::string, std::vector<uint8_t>>;
  static std::map<std::string, Ns> &store() {
    static std::map<std::string, Ns> s;
    return s;
  }
  std::string ns_;
};
