// The gauge firmware on the PC: its own setup() and loop() (car_gauge_v1.ino),
// its own screens (ui*.cpp) in an SDL window, fed a recorded drive.
//
//   sim/run.sh [drive_....csv] [trip_....gpx]
//   gauge-sim --shots DIR ...   screenshots of every screen, then quit
//                               (sim/screenshots.sh, sim_shots.cpp)
//
// Keys in the window:
//   space      pause / go on          1 2 3   1x, 5x, 20x speed
//   left right 30 s back / forward     u       a PC on USB (the bench page)
//   q, esc     quit
// The mouse is the finger: click for a tap, drag sideways for a swipe.

#include <SDL2/SDL.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "Arduino.h"
#include "sim_replay.h"
void setup();
void loop();
void shots_init(const char *dir);   // sim_shots.cpp
bool shots_on();
void shots_step(double replayMs);
extern bool simUsbHost;

// ---- the simulation's clock -------------------------------------------------
using Clock = std::chrono::steady_clock;
static Clock::time_point wallRef = Clock::now();
static double simRefMs = 0.0;
static double speed = 1.0;
static bool paused = false;

static double simNowMs() {
  if (paused) return simRefMs;
  const double wall = std::chrono::duration<double, std::milli>(Clock::now() - wallRef).count();
  return simRefMs + wall * speed;
}
static void rebase() {   // before changing speed or pausing: keep the time continuous
  simRefMs = simNowMs();
  wallRef = Clock::now();
}

uint32_t millis() { return (uint32_t)simNowMs(); }
uint32_t micros() { return (uint32_t)(simNowMs() * 1000.0); }
void delay(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// ---- the drive on that clock ------------------------------------------------
// Replay time = uptime minus this: the drive starts a few seconds after
// "power on", so the boot splash and its hold are seen first.
static double replayOffsetMs = 4000.0;

static void seek(double byMs) {
  replayOffsetMs -= byMs;
  if (byMs < 0) replay_rewind();   // fed again from the start, up to the new time
  std::printf("[sim] replay at %.0f s\n", (simNowMs() - replayOffsetMs) / 1000.0);
}

static int onEvent(void *, SDL_Event *e) {
  if (e->type != SDL_KEYDOWN || e->key.repeat) return 0;
  switch (e->key.keysym.sym) {
    case SDLK_SPACE: rebase(); paused = !paused; std::printf("[sim] %s\n", paused ? "paused" : "running"); break;
    case SDLK_1: rebase(); speed = 1.0; std::printf("[sim] 1x\n"); break;
    case SDLK_2: rebase(); speed = 5.0; std::printf("[sim] 5x\n"); break;
    case SDLK_3: rebase(); speed = 20.0; std::printf("[sim] 20x\n"); break;
    case SDLK_RIGHT: seek(30000.0); break;
    case SDLK_LEFT: seek(-30000.0); break;
    case SDLK_u: simUsbHost = !simUsbHost; std::printf("[sim] PC on USB: %s\n", simUsbHost ? "yes" : "no"); break;
    case SDLK_q:
    case SDLK_ESCAPE: std::exit(0);
  }
  return 0;
}

static bool endsWith(const std::string &s, const char *suffix) {
  const size_t n = std::strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i];
    if (a == "--shots" && i + 1 < argc) { shots_init(argv[++i]); continue; }
    const bool ok = endsWith(a, ".csv") ? replay.loadCsv(a) : endsWith(a, ".gpx") ? replay.loadGpx(a) : false;
    std::printf("[sim] %s: %s\n", a.c_str(), ok ? "loaded" : "could not read (a plain .csv or .gpx?)");
  }
  replay.finish();
  if (replay.lengthMs)
    std::printf("[sim] the drive: %u rows, %u fixes, %.0f min\n", (unsigned)replay.rows.size(),
                (unsigned)replay.fixes.size(), replay.lengthMs / 60000.0);
  else
    std::printf("[sim] no drive given: the gauge on the bench, no car (u: a PC on USB)\n");

  setup();
  SDL_AddEventWatch(onEvent, nullptr);
  if (shots_on()) { rebase(); speed = 20.0; }   // nobody watching: no need to wait
  // SIM_SPEED=20: replay faster from the start - with SIM_INVALIDATE, to read
  // a whole drive's redraws without sitting through it.
  if (const char *s = std::getenv("SIM_SPEED")) { rebase(); speed = std::atof(s) > 0 ? std::atof(s) : 1.0; }
  for (;;) {
    const double t = simNowMs() - replayOffsetMs;
    if (t >= 0) replay_feed((uint32_t)t);
    loop();
    shots_step(t);
  }
}
