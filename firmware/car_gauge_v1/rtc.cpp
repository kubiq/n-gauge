// ============================================================================
// rtc.cpp  —  PCF85063 driver. See rtc.h for why this exists at all.
// ============================================================================
#include "rtc.h"
#include "config.h"
#include "calc.h"       // calc_timegm; timegm() is absent from this newlib
#include "gps.h"        // gps.timeSet: the only source we trust to seed the chip
#include <Arduino.h>
#include <Wire.h>
#include <time.h>
#include <sys/time.h>

RtcState rtc;

#if RTC_ENABLE

// PCF85063 register map. Only two of these matter to us:
//   0x00 Control_1: bit5 STOP, bit1 12_24 (0 = 24 h), bit0 CAP_SEL
//   0x04 Seconds:   bit7 OS - the oscillator-stop flag
//
// OS is the integrity check the chip gives us for free: the hardware sets it
// when the oscillator has stopped, i.e. the chip lost power, and it stays set
// until something writes the seconds register. So OS==1 means "the time in
// here is garbage" and is the difference between seeding the clock with a real
// time and seeding it with whatever was in the registers at brown-out.
static const uint8_t REG_CTRL1 = 0x00;
static const uint8_t REG_SEC   = 0x04;
static const uint8_t CTRL1_STOP  = 0x20;
static const uint8_t CTRL1_12_24 = 0x02;
static const uint8_t SEC_OS      = 0x80;

static inline uint8_t bcd2bin(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
static inline uint8_t bin2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

// A STOP between setting the pointer and reading is deliberate. The PCF85063
// keeps its auto-incrementing address pointer across a stop, so this works,
// and it avoids relying on repeated-start support in the Wire implementation.
static bool readRegs(uint8_t reg, uint8_t *buf, size_t n) {
  Wire.beginTransmission(RTC_I2C_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom((int)RTC_I2C_ADDR, (int)n) != (int)n) return false;
  for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)Wire.read();
  return true;
}

static bool writeRegs(uint8_t reg, const uint8_t *buf, size_t n) {
  Wire.beginTransmission(RTC_I2C_ADDR);
  Wire.write(reg);
  for (size_t i = 0; i < n; i++) Wire.write(buf[i]);
  return Wire.endTransmission() == 0;
}

// Returns false if the chip did not answer, the time is not a valid date, or
// the oscillator had stopped. `os` reports that last case separately, because
// "chip missing" and "chip present but was unpowered" need different messages.
static bool rtcReadEpoch(time_t *out, bool *os) {
  uint8_t r[7];
  if (os) *os = false;
  if (!readRegs(REG_SEC, r, sizeof(r))) return false;

  // Keep decoding even when OS is set. The registers still hold the time at
  // the moment the oscillator stopped, i.e. the last time this device knew -
  // which is worth reporting rather than discarding. The caller decides what
  // a stale time is good for; this function only says whether it parsed.
  if (r[0] & SEC_OS) {
    if (os) *os = true;
  }

  const int sec = bcd2bin(r[0] & 0x7F);
  const int min = bcd2bin(r[1] & 0x7F);
  const int hr  = bcd2bin(r[2] & 0x3F);
  const int day = bcd2bin(r[3] & 0x3F);
  // r[4] is the weekday, which we neither need nor trust - it is a separate
  // counter the chip does not derive from the date, so it can disagree with it.
  const int mon = bcd2bin(r[5] & 0x1F);
  const int yr  = 2000 + bcd2bin(r[6]);

  // Garbage registers can still be valid BCD, so range-check before trusting.
  if (sec > 59 || min > 59 || hr > 23) return false;
  if (day < 1 || day > 31 || mon < 1 || mon > 12) return false;

  const long long e = calc_timegm(yr, (unsigned)mon, (unsigned)day,
                                  (unsigned)hr, (unsigned)min, (unsigned)sec);
  if (e <= 0) return false;
  *out = (time_t)e;
  return true;
}

static bool rtcWriteEpoch(time_t e) {
  struct tm t;
  gmtime_r(&e, &t);                     // the chip holds UTC; see rtc.h
  const int year = t.tm_year + 1900;
  if (year < 2000 || year > 2099) return false;   // two BCD digits only

  uint8_t c1;
  if (!readRegs(REG_CTRL1, &c1, 1)) return false;

  // STOP the counter across the seven-register update. Without it a carry
  // landing mid-write leaves a mixed time - e.g. the new second with the old
  // minute - which is a once-a-minute chance of writing a wrong time and
  // believing it.
  uint8_t stopped = (uint8_t)(c1 | CTRL1_STOP);
  if (!writeRegs(REG_CTRL1, &stopped, 1)) return false;

  const uint8_t r[7] = {
    bin2bcd((uint8_t)t.tm_sec),         // OS cleared by writing this register
    bin2bcd((uint8_t)t.tm_min),
    bin2bcd((uint8_t)t.tm_hour),
    bin2bcd((uint8_t)t.tm_mday),
    (uint8_t)t.tm_wday,
    bin2bcd((uint8_t)(t.tm_mon + 1)),
    bin2bcd((uint8_t)(year - 2000)),
  };
  const bool ok = writeRegs(REG_SEC, r, sizeof(r));

  // Restart the counter whatever happened above: leaving STOP set would freeze
  // the clock entirely, which is worse than the failed write we came here for.
  uint8_t running = (uint8_t)(c1 & ~CTRL1_STOP);
  writeRegs(REG_CTRL1, &running, 1);
  return ok;
}

#endif  // RTC_ENABLE

bool clock_valid() {
  return time(nullptr) > CLOCK_SANE_EPOCH;
}

bool rtc_set_from_epoch(long long epochUtc) {
  // Both bounds matter. The lower one is the same gate every other clock
  // source passes; the upper one is not decoration - the PCF85063 stores the
  // year as two BCD digits, so anything past 2099 cannot be written at all and
  // would be silently mangled into some other year.
  if (epochUtc <= (long long)CLOCK_SANE_EPOCH) return false;
  if (epochUtc >= (long long)CLOCK_MAX_EPOCH)  return false;

  const time_t e = (time_t)epochUtc;
  struct timeval tv = { .tv_sec = e, .tv_usec = 0 };
  settimeofday(&tv, nullptr);

#if RTC_ENABLE
  // Write the chip too, or this lasts only until the next reboot - which
  // defeats the point, since the reason to set it by hand is that no GNSS fix
  // is coming to re-set it.
  if (rtc.present) {
    if (rtcWriteEpoch(e)) {
      rtc.seeded    = true;
      rtc.timeValid = true;
      rtc.driftSec  = 0;
      Serial.println(F("[OK] clock set from settings page, written to RTC"));
    } else {
      // The system clock is still set, so say what did and did not happen
      // rather than reporting a flat failure.
      Serial.println(F("[ERR] clock set, but the RTC write failed - it will not survive a reboot"));
    }
  }
#endif
  return true;
}

void rtc_init() {
#if !RTC_ENABLE
  return;
#else
  Wire.beginTransmission(RTC_I2C_ADDR);
  rtc.present = (Wire.endTransmission() == 0);
  if (!rtc.present) {
    Serial.println(F("[ERR] PCF85063 not answering at 0x51 - clock waits for GNSS"));
    return;
  }

  // Force 24-hour mode and make sure the counter is running. CAP_SEL (bit 0)
  // is left exactly as found: it selects the quartz load capacitance for the
  // crystal this board actually has, and guessing it wrong costs accuracy.
  uint8_t c1;
  if (readRegs(REG_CTRL1, &c1, 1)) {
    const uint8_t want = (uint8_t)(c1 & ~(CTRL1_12_24 | CTRL1_STOP));
    if (want != c1) writeRegs(REG_CTRL1, &want, 1);
  }

  time_t e = 0;
  bool os = false;
  if (!rtcReadEpoch(&e, &os)) {
    Serial.println(F("[..] RTC registers are not a valid date - waiting for GNSS"));
    return;
  }

#if RTC_SELFTEST
  // Bring-up only, and DESTRUCTIVE: it overwrites whatever the chip holds.
  //
  // It proves the write path at the only moment that is free - when OS says
  // the contents are stale anyway - which was otherwise unverifiable on the
  // bench, since a real write needs a GNSS fix and therefore sky. It did its
  // job on 2026-09-24: a later boot read back 2000-01-01T00:02:12Z, the
  // sentinel plus elapsed time, proving both the write and that the
  // oscillator runs.
  //
  // Left OFF because it destroys the retained last-known time reported below,
  // and because a diagnostic that silently changes behaviour is exactly the
  // kind of flag that gets left on by accident.
  if (os) {
    const time_t sentinel = 946684800L;             // 2000-01-01T00:00:00Z
    time_t back = 0;
    if (rtcWriteEpoch(sentinel) && rtcReadEpoch(&back, nullptr) &&
        back == sentinel) {
      Serial.println(F("[OK] RTC read/write verified"));
    } else {
      Serial.printf("[ERR] RTC write/readback FAILED (wrote %ld, read %ld)\n",
                    (long)sentinel, (long)back);
    }
    return;
  }
#endif

  // The chip lost power, but its registers still hold the moment the
  // oscillator stopped - the last time this device knew. Report it and stop.
  //
  // Deliberately NOT used to seed the clock. It is a genuine lower bound on
  // "now", but it is wrong by however long the power was off, which could be
  // days, and a plausible-looking wrong date is worse than no date: it looks
  // real in a spreadsheet and silently misfiles a drive. log_csv.cpp already
  // makes exactly this call for the 1970 case, and this is the same argument.
  //
  // Nor can we have it both ways. Any write clears OS, so storing the retained
  // value back would destroy the only evidence that it is stale, and the next
  // boot would trust it completely. OS is the whole signal.
  if (os) {
    char stale[24];
    struct tm stm;
    gmtime_r(&e, &stm);
    strftime(stale, sizeof(stale), "%Y-%m-%dT%H:%M:%SZ", &stm);
    Serial.printf("[..] RTC lost power; last time it knew was %s"
                  " - not trusted, waiting for GNSS\n", stale);
    return;
  }

  // Sanity-gate against the same epoch the rest of the firmware uses. A chip
  // that answers and reads 1999 is worse than one that admits it is lost.
  // Print what it actually read, not just that the value was rejected. A bare
  // "too old" tells you nothing about whether the chip is keeping time badly,
  // holding the self-test sentinel, or decoding to nonsense.
  char had[24];
  struct tm htm;
  gmtime_r(&e, &htm);
  strftime(had, sizeof(had), "%Y-%m-%dT%H:%M:%SZ", &htm);

  if (e <= CLOCK_SANE_EPOCH) {
    Serial.printf("[..] RTC reads %s, older than CLOCK_SANE_EPOCH"
                  " - waiting for GNSS\n", had);
    return;
  }

  // A system clock that survived (deep sleep, a reset) is kept only while it
  // agrees with the chip. Through a deep sleep it runs on the ESP32's RC slow
  // clock, a few percent out - minutes per parked hour - while the chip runs
  // on a quartz crystal; it was the system clock that "won" here, and
  // rtc_task() then wrote its drift into the chip. Within a second, the system
  // clock is kept for its sub-second part, which the chip does not have.
  rtc.timeValid = true;
  if (clock_valid()) {
    const long long diff = (long long)time(nullptr) - (long long)e;
    if (diff > -RTC_TRUST_SYS_SEC && diff < RTC_TRUST_SYS_SEC) {
      Serial.println(F("[..] system clock agrees with the RTC; kept"));
      return;
    }
    Serial.printf("[..] system clock %+lld s from the RTC - taking the RTC\n", diff);
  }

  struct timeval tv = { .tv_sec = e, .tv_usec = 0 };
  settimeofday(&tv, nullptr);

  char buf[24];
  struct tm tm;
  gmtime_r(&e, &tm);
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
  Serial.printf("[OK] clock seeded from RTC: %s\n", buf);
#endif
}

void rtc_task() {
#if !RTC_ENABLE
  return;
#else
  if (!rtc.present) return;

  // Only GNSS time is worth writing. Seeding the chip from a clock the chip
  // itself provided would just launder its own drift back into it and make the
  // drift figure below meaningless.
  if (!gps.timeSet) return;

  static uint32_t lastCheckMs = 0;
  const uint32_t now = millis();
  if (lastCheckMs != 0 && (int32_t)(now - lastCheckMs) < (int32_t)RTC_SYNC_CHECK_MS) {
    return;
  }
  lastCheckMs = now ? now : 1;

  const time_t sys = time(nullptr);
  time_t inChip = 0;
  bool os = false;
  const bool readOk = rtcReadEpoch(&inChip, &os);

  if (readOk) {
    rtc.driftSec = (long)(inChip - sys);
    if (rtc.driftSec > -RTC_DRIFT_TOL_SEC && rtc.driftSec < RTC_DRIFT_TOL_SEC) {
      return;                           // close enough; leave the chip alone
    }
  }

  if (!rtcWriteEpoch(sys)) {
    Serial.println(F("[ERR] RTC write failed"));
    return;
  }

  // Log the correction, not just the fact of it. Over a few drives this is the
  // only way we learn whether the chip keeps decent time or drifts badly - and
  // if it drifts, CAP_SEL is the first thing to suspect.
  if (!rtc.seeded) {
    rtc.seeded = true;
    Serial.println(F("[OK] RTC set from GNSS"));
  } else {
    Serial.printf("[..] RTC corrected by %+ld s\n", rtc.driftSec);
  }
  rtc.timeValid = true;
#endif
}
