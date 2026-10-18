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
#include "settings.h"
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
// The over-temperature warning, on the track screen: the made-up session's
// oil passes the gauge's warning in its last laps, so this one waits longer.
static bool oilHot() { return obd.haveOil && obd.oilTempC >= settings.oilWarnC + 2; }
struct Shot { const char *name; GaugeView view; bool (*ready)(); uint32_t giveUpMs; };
static const Shot SHOTS[] = {
    {"oil", VIEW_OIL, anytime, 30000},       {"boost", VIEW_BOOST, boosting, 30000},
    {"gmeter", VIEW_GMETER, cornering, 30000}, {"tyres", VIEW_TPMS, anytime, 30000},
    {"gps", VIEW_GPS, anytime, 30000},       {"track", VIEW_TRACK, anytime, 30000},
    {"oilhot", VIEW_TRACK, oilHot, 300000},
};
static const uint32_t SETTLE_MS = 1500;    // after switching: values, the g-meter's trail

// One object rendered into a buffer of its own: a 466x466 snapshot is
// 870 KB, far more than LVGL's pool (the gauge's 128 KB, kept as it is so the
// sim runs out where the gauge would). ARGB, BGRA in memory.
struct Snap {
  std::vector<uint8_t> mem;
  lv_draw_buf_t buf;
  bool take(lv_obj_t *obj, uint32_t w, uint32_t h) {
    const uint32_t stride = lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_ARGB8888);
    mem.assign(stride * h + LV_DRAW_BUF_ALIGN, 0);
    uint8_t *data = (uint8_t *)lv_draw_buf_align(mem.data(), LV_COLOR_FORMAT_ARGB8888);
    lv_draw_buf_init(&buf, w, h, LV_COLOR_FORMAT_ARGB8888, stride, data, stride * h);
    return lv_snapshot_take_to_draw_buf(obj, LV_COLOR_FORMAT_ARGB8888, &buf) == LV_RESULT_OK;
  }
  const uint8_t *px(uint32_t x, uint32_t y) const { return buf.data + y * buf.header.stride + x * 4; }
};

// The active screen as a PPM (RGB), enough for ImageMagick to take on - with
// the top layer over it, as the panel shows them: the warning rim and its
// line live there, over every screen.
static void save(const char *name) {
  lv_obj_t *scr = lv_screen_active();
  const uint32_t w = lv_obj_get_width(scr), h = lv_obj_get_height(scr);
  Snap base, top;
  if (!base.take(scr, w, h)) {
    std::printf("[shots] %s: no snapshot\n", name);
    return;
  }
  const bool haveTop = top.take(lv_layer_top(), w, h);
  const std::string path = dir + "/" + name + ".ppm";
  FILE *f = std::fopen(path.c_str(), "wb");
  if (f) {
    std::fprintf(f, "P6\n%u %u\n255\n", (unsigned)w, (unsigned)h);
    for (uint32_t y = 0; y < h; y++) {
      for (uint32_t x = 0; x < w; x++) {
        const uint8_t *b = base.px(x, y);
        uint8_t out[3] = {b[2], b[1], b[0]};   // BGRA -> RGB
        if (haveTop) {
          const uint8_t *t = top.px(x, y);
          const unsigned a = t[3];
          for (int c = 0; c < 3; c++) out[c] = (uint8_t)((t[2 - c] * a + out[c] * (255 - a)) / 255);
        }
        std::fwrite(out, 1, 3, f);
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
      if (!SHOTS[next].ready() && now - viewAt < SHOTS[next].giveUpMs) break;
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
