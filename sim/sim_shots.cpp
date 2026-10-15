// Screenshots of the gauge's screens, for the READMEs: gauge-sim --shots DIR
// (sim/screenshots.sh makes round PNGs of them). The run is steered as a
// person would: the boot screen first, then START on the track screen, and
// once a lap is being timed against a best one - past halfway, ahead of it -
// every screen in turn. Each is rendered straight from its objects
// (lv_snapshot), so it is exact whatever the window is doing. Then it quits.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "Arduino.h"
#include "laps.h"
#include "lvgl.h"
#include "obd.h"
#include "sim_replay.h"
#include "ui.h"

extern float simImu[3];   // sim_replay.cpp: the IMU as replayed

static std::string dir;
static int stage = -1;          // -1: off
static size_t next = 0;         // the next screen to take
static uint32_t viewAt = 0;     // when it was switched to (millis)

// Each screen waits for a moment that shows it at work - boost while the
// turbo is pushing, the g-meter in a hard corner - or gives up and takes
// what there is.
static bool anytime() { return true; }
static bool boosting() { return obd.haveBoost && obd.boostBar > 1.0f; }
static bool cornering() { return std::fabs(simImu[1]) > 0.8f; }
struct Shot { const char *name; GaugeView view; bool (*ready)(); };
static const Shot SHOTS[] = {
    {"oil", VIEW_OIL, anytime},    {"boost", VIEW_BOOST, boosting}, {"gmeter", VIEW_GMETER, cornering},
    {"tyres", VIEW_TPMS, anytime}, {"gps", VIEW_GPS, anytime},      {"track", VIEW_TRACK, anytime},
};
static const uint32_t SETTLE_MS = 1500;    // after switching: values, the g-meter's trail
static const uint32_t GIVE_UP_MS = 30000;  // waiting for a moment

// The active screen as a PPM (RGB): enough for ImageMagick to take on.
// Into a buffer of its own: a 466x466 snapshot is 870 KB, far more than
// LVGL's pool (the gauge's 128 KB, kept as it is so the sim runs out where
// the gauge would).
static void save(const char *name) {
  lv_obj_t *scr = lv_screen_active();
  const uint32_t w = lv_obj_get_width(scr), h = lv_obj_get_height(scr);
  const uint32_t stride = lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_ARGB8888);
  std::vector<uint8_t> mem(stride * h + LV_DRAW_BUF_ALIGN);
  lv_draw_buf_t buf;
  uint8_t *data = (uint8_t *)lv_draw_buf_align(mem.data(), LV_COLOR_FORMAT_ARGB8888);
  lv_draw_buf_init(&buf, w, h, LV_COLOR_FORMAT_ARGB8888, stride, data, stride * h);
  lv_draw_buf_t *b = &buf;
  if (lv_snapshot_take_to_draw_buf(scr, LV_COLOR_FORMAT_ARGB8888, b) != LV_RESULT_OK) {
    std::printf("[shots] %s: no snapshot\n", name);
    return;
  }
  const std::string path = dir + "/" + name + ".ppm";
  FILE *f = std::fopen(path.c_str(), "wb");
  if (f) {
    std::fprintf(f, "P6\n%u %u\n255\n", (unsigned)w, (unsigned)h);
    for (uint32_t y = 0; y < h; y++) {
      const uint8_t *row = b->data + y * b->header.stride;
      for (uint32_t x = 0; x < w; x++) {
        const uint8_t px[3] = {row[x * 4 + 2], row[x * 4 + 1], row[x * 4 + 0]};   // BGRA -> RGB
        std::fwrite(px, 1, 3, f);
      }
    }
    std::fclose(f);
    std::printf("[shots] %s\n", path.c_str());
  }
}

void shots_init(const char *d) {
  dir = d;
  stage = 0;
}

bool shots_on() { return stage >= 0; }

void shots_step(double replayMs) {
  if (stage < 0) return;
  const uint32_t now = millis();
  switch (stage) {
    case 0:   // the boot screen: the N badge, before the car answers
      if (now >= 1500) { save("wake"); stage = 1; }
      break;
    case 1:   // the drive is on: START, as in the pit lane
      if (replayMs > 100) {
        ui_set_view(VIEW_TRACK);
        laps_start();
        stage = 2;
      }
      break;
    case 2: {   // a lap against a best one, past halfway and ahead of it
      LapInfo li;
      laps_info(&li);
      // Two laps done, so the best is a flying lap rather than the out-lap.
      const bool good = li.laps >= 2 && li.state == LAP_TIMING && li.haveDelta && li.deltaMs < 0 &&
                        li.lapM > 0.5f * li.refM;
      // Without laps in the drive: the screens anyway, once it is well under way.
      const bool none = replay.fixes.empty() ? replayMs > 60000 : replayMs > replay.lengthMs;
      if (good || none) {
        ui_set_view(SHOTS[0].view);
        viewAt = now;
        stage = 3;
      }
      break;
    }
    case 3:
      if (now - viewAt < SETTLE_MS) break;
      if (!SHOTS[next].ready() && now - viewAt < GIVE_UP_MS) break;
      save(SHOTS[next].name);
      if (++next == sizeof(SHOTS) / sizeof(SHOTS[0])) {
        std::printf("[shots] done\n");
        std::exit(0);
      }
      ui_set_view(SHOTS[next].view);
      viewAt = now;
      break;
  }
}
