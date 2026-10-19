// ============================================================================
// prof.h  —  timing for FRAME_STATS builds, at no cost in the car's.
//
//   static ProfStat drawStat;
//   { PROF_SCOPE(drawStat); draw(); }    // times the block: count, sum, max
//   PROF_ADD(pixels, w * h);             // a plain counter
//
// With FRAME_STATS 0 both expand to nothing - their arguments are not even
// compiled, so the stats variables themselves can live under #if FRAME_STATS
// and the code that feeds them needs no #if of its own.
// ============================================================================
#pragma once
#include <stdint.h>
#include "config.h"

#if FRAME_STATS
#include <Arduino.h>   // micros

// Samples of one duration: how many, their total and the longest, in us.
struct ProfStat {
  uint32_t n = 0, sumUs = 0, maxUs = 0;
  void add(uint32_t us) {
    n++;
    sumUs += us;
    if (us > maxUs) maxUs = us;
  }
  uint32_t avgUs() const { return n ? sumUs / n : 0; }
};

// Adds the time from its construction to the end of its scope to a ProfStat.
class ProfScope {
 public:
  explicit ProfScope(ProfStat &s) : s_(s), t0_(micros()) {}
  ~ProfScope() { s_.add(micros() - t0_); }
  ProfScope(const ProfScope &) = delete;
  ProfScope &operator=(const ProfScope &) = delete;
 private:
  ProfStat      &s_;
  const uint32_t t0_;
};

#define PROF_CAT_(a, b) a##b
#define PROF_CAT(a, b)  PROF_CAT_(a, b)
#define PROF_SCOPE(stat)    ProfScope PROF_CAT(profScope_, __LINE__)(stat)
#define PROF_ADD(var, n)    ((var) += (n))
#else
#define PROF_SCOPE(stat)    ((void)0)
#define PROF_ADD(var, n)    ((void)0)
#endif
