// ============================================================================
// test_obd.cpp  —  the OBD reply parsing in calc.h: ISO-TP reassembly, the
// multi-PID walk and the Mode 01 formulas. obd.cpp feeds these from the CAN
// driver; here they get the frames the car sends, and the ones that broke
// them before (an interloping First Frame, a short last frame).
// ============================================================================
#include "test.h"
#include <vector>
#include "../firmware/car_gauge_v1/calc.h"

// ---- ISO-TP -------------------------------------------------------------------

struct Rx {
  CalcIsoEvent ev;
  std::vector<uint8_t> payload;
};

// One frame. `data` is always 8 bytes, as the driver's buffer is; dlc says how
// many are the frame's own. Bytes past dlc are filled with 0xEE - the stale
// tail of an earlier frame that must never reach a payload.
static Rx rx(CalcIsoTp &t, uint32_t id, std::vector<uint8_t> frame, uint32_t now = 0) {
  uint8_t d[8];
  memset(d, 0xEE, sizeof(d));
  memcpy(d, frame.data(), frame.size());
  const uint8_t *p = nullptr;
  uint16_t n = 0;
  Rx r;
  r.ev = calc_isotp_rx(&t, id, d, (uint8_t)frame.size(), now, &p, &n);
  if (r.ev == CALC_ISO_SINGLE || r.ev == CALC_ISO_DONE) r.payload.assign(p, p + n);
  return r;
}

static void single_frame_gives_its_payload() {
  CalcIsoTp t;
  // 41 05 7B: coolant 83 °C, as the ECU answers PID 0x05.
  Rx r = rx(t, 0x7E8, {0x03, 0x41, 0x05, 0x7B, 0x00, 0x00, 0x00, 0x00});
  CHECK_EQ(r.ev, CALC_ISO_SINGLE);
  CHECK(r.payload == std::vector<uint8_t>({0x41, 0x05, 0x7B}));
}

// The oil DID's reply: 45 bytes, a First Frame and six Consecutive Frames,
// the last one short and unpadded.
static std::vector<uint8_t> oilPayload() {
  std::vector<uint8_t> p = {0x62, 0xE0, 0x01};
  for (int i = 0; i < 42; i++) p.push_back((uint8_t)(0x30 + i));
  return p;
}

static void a_multi_frame_reply_reassembles() {
  CalcIsoTp t;
  const std::vector<uint8_t> want = oilPayload();
  std::vector<uint8_t> ff = {0x10, 45};
  ff.insert(ff.end(), want.begin(), want.begin() + 6);
  CHECK_EQ(rx(t, 0x7E8, ff).ev, CALC_ISO_FIRST);
  size_t at = 6;
  for (uint8_t seq = 1; at < want.size(); seq++) {
    std::vector<uint8_t> cf = {(uint8_t)(0x20 | (seq & 0x0F))};
    for (int k = 0; k < 7 && at < want.size(); k++) cf.push_back(want[at++]);
    Rx r = rx(t, 0x7E8, cf);
    if (at < want.size()) CHECK_EQ(r.ev, CALC_ISO_NONE);
    else {
      CHECK_EQ(r.ev, CALC_ISO_DONE);
      // The last frame was 5 bytes long; its 0xEE tail stayed out.
      CHECK(r.payload == want);
    }
  }
  CHECK(!t.active);
}

// The field bug: a TPMS First Frame in the middle of the oil transfer used to
// reset the buffer, and the oil frames then completed as a "TPMS" payload.
static void an_interloping_first_frame_is_refused() {
  CalcIsoTp t;
  rx(t, 0x7E8, {0x10, 0x0A, 0x62, 0xE0, 0x01, 1, 2, 3});
  CHECK_EQ(rx(t, 0x7A8, {0x10, 0x14, 0x62, 0xC0, 0x0B, 9, 9, 9}).ev, CALC_ISO_ABORT);
  // The oil transfer is untouched and completes.
  Rx r = rx(t, 0x7E8, {0x21, 4, 5, 6, 7});
  CHECK_EQ(r.ev, CALC_ISO_DONE);
  CHECK(r.payload == std::vector<uint8_t>({0x62, 0xE0, 0x01, 1, 2, 3, 4, 5, 6, 7}));
}

static void a_consecutive_frame_from_another_module_is_refused() {
  CalcIsoTp t;
  rx(t, 0x7E8, {0x10, 0x0A, 0x62, 0xE0, 0x01, 1, 2, 3});
  CHECK_EQ(rx(t, 0x7A8, {0x21, 9, 9, 9, 9, 9, 9, 9}).ev, CALC_ISO_ABORT);
  CHECK(t.active);
  CHECK_EQ(rx(t, 0x7E8, {0x21, 4, 5, 6, 7}).ev, CALC_ISO_DONE);
}

static void a_skipped_frame_ends_the_transfer() {
  CalcIsoTp t;
  rx(t, 0x7E8, {0x10, 0x14, 0x62, 0xE0, 0x01, 1, 2, 3});
  CHECK_EQ(rx(t, 0x7E8, {0x22, 1, 1, 1, 1, 1, 1, 1}).ev, CALC_ISO_ABORT);   // 21 missing
  CHECK(!t.active);
  CHECK_EQ(rx(t, 0x7E8, {0x21, 1, 1, 1, 1, 1, 1, 1}).ev, CALC_ISO_NONE);    // too late
}

// More than 15 Consecutive Frames: the sequence number wraps 0x2F -> 0x20.
static void the_sequence_number_wraps() {
  CalcIsoTp t;
  rx(t, 0x7E8, {0x10, 96, 0, 0, 0, 0, 0, 0});              // 6 + 13 x 7 = 97 >= 96
  for (int seq = 1; seq <= 13; seq++) {
    Rx r = rx(t, 0x7E8, {(uint8_t)(0x20 | (seq & 0x0F)), 1, 2, 3, 4, 5, 6, 7});
    CHECK_EQ(r.ev, seq < 13 ? CALC_ISO_NONE : CALC_ISO_DONE);
  }
  CalcIsoTp u;
  rx(u, 0x7E8, {0x10, 0xFF, 0, 0, 0, 0, 0, 0});           // clamped to the buffer
  CHECK_EQ(u.expected, CALC_ISOTP_BUF);
  for (int seq = 1; seq <= 16; seq++) {
    const Rx r = rx(u, 0x7E8, {(uint8_t)(0x20 | (seq & 0x0F)), 1, 2, 3, 4, 5, 6, 7});
    if (seq == 16) CHECK_EQ(r.ev, CALC_ISO_NONE);         // 0x20: accepted, not aborted
  }
}

static void a_stalled_transfer_expires() {
  CalcIsoTp t;
  rx(t, 0x7E8, {0x10, 0x14, 0x62, 0xE0, 0x01, 1, 2, 3}, 1000);
  CHECK(!calc_isotp_expire(&t, 1300, 300));
  CHECK(calc_isotp_expire(&t, 1301, 300));
  CHECK(!t.active);
  CHECK(!calc_isotp_expire(&t, 9999, 300));               // once
}

static void frames_too_short_or_too_long_are_safe() {
  CalcIsoTp t;
  CHECK_EQ(rx(t, 0x7E8, {0x03}).ev, CALC_ISO_NONE);
  // A Single Frame claiming more than its frame holds gets what is there.
  Rx r = rx(t, 0x7E8, {0x07, 0x41, 0x0D});
  CHECK_EQ(r.ev, CALC_ISO_SINGLE);
  CHECK_EQ(r.payload.size(), 2);
}

// ---- the multi-PID walk ---------------------------------------------------------

struct Seen { uint8_t pid; std::vector<uint8_t> d; };

static std::vector<Seen> walk(std::vector<uint8_t> p) {
  std::vector<Seen> out;
  calc_mode01_walk(p.data(), (uint16_t)p.size(), [&](uint8_t pid, const uint8_t *d, uint16_t n) {
    out.push_back({pid, std::vector<uint8_t>(d, d + n)});
  });
  return out;
}

// The fast request's reply: MAP, speed, absolute load, pedal, rpm, fuel rate.
static void a_multi_pid_reply_splits_by_pid() {
  auto s = walk({0x41, 0x0B, 0xA0, 0x0D, 0x50, 0x43, 0x01, 0x2C, 0x49, 0x25,
                 0x0C, 0x1A, 0xF8, 0x5E, 0x01, 0x90});
  CHECK_EQ(s.size(), 6);
  CHECK(s[0].pid == 0x0B && s[0].d == std::vector<uint8_t>({0xA0}));
  CHECK(s[2].pid == 0x43 && s[2].d == std::vector<uint8_t>({0x01, 0x2C}));
  CHECK(s[4].pid == 0x0C && s[4].d == std::vector<uint8_t>({0x1A, 0xF8}));
  CHECK(s[5].pid == 0x5E && s[5].d == std::vector<uint8_t>({0x01, 0x90}));
}

// An unknown PID takes the rest - right for a single-PID reply, and a walk
// that stops rather than misreading every PID after it.
static void an_unknown_pid_takes_the_rest() {
  auto s = walk({0x41, 0x0D, 0x50, 0x99, 1, 2, 3});
  CHECK_EQ(s.size(), 2);
  CHECK(s[1].pid == 0x99 && s[1].d.size() == 3);
}

static void a_truncated_reply_hands_over_what_is_there() {
  auto s = walk({0x41, 0x0D, 0x50, 0x0C, 0x1A});         // rpm cut to one byte
  CHECK_EQ(s.size(), 2);
  CHECK(s[1].pid == 0x0C && s[1].d.size() == 1);
}

// ---- Mode 01 formulas (SAE J1979) ------------------------------------------------

static float val(uint8_t pid, std::vector<uint8_t> d) {
  float v = -12345.0f;
  if (!calc_mode01_value(pid, d.data(), (uint16_t)d.size(), &v)) return -99999.0f;
  return v;
}

static void formulas_match_sae() {
  CHECK_EQ(val(0x05, {0x7B}), 83);                 // coolant A - 40
  CHECK_EQ(val(0x0F, {0x00}), -40);                // intake air, the floor
  CHECK_EQ(val(0x0D, {0x50}), 80);                 // km/h
  CHECK_EQ(val(0x0C, {0x1A, 0xF8}), 1726);         // (256A + B) / 4
  CHECK_EQ(val(0x0C, {0x00, 0x03}), 0);            // whole rpm, truncated
  CHECK_NEAR(val(0x0E, {0x80}), 0.0f, 0.001f);     // A/2 - 64
  CHECK_NEAR(val(0x43, {0x01, 0x2C}), 117.65f, 0.01f);   // absolute load can exceed 100
  CHECK_NEAR(val(0x42, {0x36, 0xB0}), 14.0f, 0.001f);    // 14000 mV
  CHECK_NEAR(val(0x5E, {0x01, 0x90}), 20.0f, 0.001f);    // 400 / 20 L/h
  CHECK_NEAR(val(0x34, {0x80, 0x00}), 1.0f, 0.0001f);    // lambda 1.000
  CHECK_NEAR(val(0x44, {0x7A, 0xE1}), 0.96f, 0.001f);
  CHECK_NEAR(val(0x2F, {0xFF}), 100.0f, 0.001f);
  CHECK_NEAR(val(0x3C, {0x1F, 0x40}), 760.0f, 0.001f);   // 8000 / 10 - 40
  CHECK_NEAR(val(0x59, {0x07, 0xD0}), 200.0f, 0.001f);   // 2000 x 10 kPa = 200 bar
  CHECK_EQ(val(0x33, {0x62}), 98);                 // baro kPa, raw
  CHECK_EQ(val(0x49, {0x25}), 37);                 // pedal raw, foot off
}

static void short_or_unknown_is_no_value() {
  CHECK_EQ(val(0x0C, {0x1A}), -99999);             // needs two bytes
  CHECK_EQ(val(0x05, {}), -99999);
  CHECK_EQ(val(0x99, {1, 2}), -99999);
}

int main() {
  printf("test_obd\n");
  TEST(single_frame_gives_its_payload);
  TEST(a_multi_frame_reply_reassembles);
  TEST(an_interloping_first_frame_is_refused);
  TEST(a_consecutive_frame_from_another_module_is_refused);
  TEST(a_skipped_frame_ends_the_transfer);
  TEST(the_sequence_number_wraps);
  TEST(a_stalled_transfer_expires);
  TEST(frames_too_short_or_too_long_are_safe);
  TEST(a_multi_pid_reply_splits_by_pid);
  TEST(an_unknown_pid_takes_the_rest);
  TEST(a_truncated_reply_hands_over_what_is_there);
  TEST(formulas_match_sae);
  TEST(short_or_unknown_is_no_value);
  TEST_SUMMARY();
}
