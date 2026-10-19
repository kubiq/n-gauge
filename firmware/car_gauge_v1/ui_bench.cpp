// ============================================================================
// ui_bench.cpp  —  render-cost measurements, on the bench only (FRAME_STATS).
// See ui_priv.h.
// ============================================================================
#include "ui_priv.h"

#if FRAME_STATS
#include "settings.h"       // the views to time, in the cycle order
#include <Arduino.h>        // micros, Serial - ui_bench() only
#include <esp_task_wdt.h>
#include <math.h>

// Render cost per screen, without anyone swiping: a full-screen redraw, and a
// small 40x40 area like a moving dot leaves behind. Each is timed through
// lv_refr_now(), so it includes the QSPI flush. Run twice - notification rim
// forced on, then off - because the rim sits on the top layer over the whole
// panel and so takes part in every redraw on every screen.
static uint32_t benchRefr(const lv_area_t *a) {
  if (a) lv_obj_invalidate_area(lv_screen_active(), a);
  else   lv_obj_invalidate(lv_screen_active());
  const uint32_t t0 = micros();
  lv_refr_now(NULL);
  return micros() - t0;
}

void ui_bench() {
  lv_obj_t *const noticeRing = ui_notice_ring();
  const GaugeView was = ui_current_view();
  const bool ringWasHidden = lv_obj_has_flag(noticeRing, LV_OBJ_FLAG_HIDDEN);
  const lv_area_t small = { 300, 260, 339, 299 };   // 40x40, off-centre
  for (int pass = 0; pass < 2; pass++) {
    if (pass == 0) lv_obj_clear_flag(noticeRing, LV_OBJ_FLAG_HIDDEN);
    else           lv_obj_add_flag(noticeRing, LV_OBJ_FLAG_HIDDEN);
    Serial.printf("[BENCH] rim %s\n", pass == 0 ? "ON" : "OFF");
    for (const char *p = settings.views; *p; p++) {
      const GaugeView v = (GaugeView)(*p - '0');
      ui_set_view(v);
      benchRefr(NULL);                                 // settle the load
      uint32_t full = 0, part = 0;
      for (int k = 0; k < 3; k++)  full += benchRefr(NULL);
      for (int k = 0; k < 20; k++) part += benchRefr(&small);
      Serial.printf("[BENCH]   view %d: full %lu us, 40x40 %lu us\n",
                    (int)v, (unsigned long)(full / 3),
                    (unsigned long)(part / 20));
      esp_task_wdt_reset();
    }
  }
  // A real g-meter frame: the dot circling at ~0.8 g, label changing, trail
  // shuffling - each step its own refresh, as at run time.
  if (lv_obj_t *const gmScr = ui_screen(VIEW_GMETER)) {
    ui_set_view(VIEW_GMETER);
    auto live = [](uint32_t *mx) {
      benchRefr(NULL);
      uint32_t sum = 0; *mx = 0;
      const int N = 60;
      for (int k = 0; k < N; k++) {
        const float a = k * 0.21f;
        ui_update_gmeter(0.8f * cosf(a), 0.8f * sinf(a), 1.0f + 0.2f * sinf(3 * a));
        const uint32_t t0 = micros();
        lv_refr_now(NULL);
        const uint32_t us = micros() - t0;
        sum += us; if (us > *mx) *mx = us;
        delay(16);                     // let the trail timer tick now and then
      }
      esp_task_wdt_reset();
      return sum / N;
    };
    uint32_t mx;
    for (int pass = 0; pass < 2; pass++) {
      if (pass == 0) lv_obj_clear_flag(noticeRing, LV_OBJ_FLAG_HIDDEN);
      else           lv_obj_add_flag(noticeRing, LV_OBJ_FLAG_HIDDEN);
      const uint32_t avg = live(&mx);
      Serial.printf("[BENCH] g-meter live, rim %s: %lu us/frame avg, %lu max\n",
                    pass == 0 ? "ON" : "OFF", (unsigned long)avg,
                    (unsigned long)mx);
    }
    // Cost of one unchanged patch, by size and place: is there a fixed cost
    // per dirty area, and does it depend on what lies under it?
    {
      lv_area_t lab, dot, tiny, blank;
      lv_obj_get_coords(ui_gmeter_label(), &lab);
      lv_obj_get_coords(ui_gmeter_dot(), &dot);
      tiny  = { 232, 232, 233, 233 };             // 2x2 in the middle
      blank = { 20, 20, 59, 35 };                 // 40x16 corner, off the backdrop
      const lv_area_t *as[] = { &lab, &dot, &tiny, &blank };
      const char *nm[] = { "label", "dot", "2x2", "40x16 corner" };
      for (int i = 0; i < 4; i++) {
        uint32_t sum = 0;
        for (int k = 0; k < 20; k++) sum += benchRefr(as[i]);
        Serial.printf("[BENCH] g-meter live patch %s %dx%d: %lu us\n", nm[i],
                      (int)lv_area_get_width(as[i]), (int)lv_area_get_height(as[i]),
                      (unsigned long)(sum / 20));
      }
      // And an empty refresh: nothing invalidated at all.
      uint32_t sum = 0;
      for (int k = 0; k < 20; k++) {
        const uint32_t t0 = micros(); lv_refr_now(NULL); sum += micros() - t0;
      }
      Serial.printf("[BENCH] g-meter live empty refresh: %lu us\n",
                    (unsigned long)(sum / 20));
    }
    // Rim still off. Same sequence with each object hidden in turn.
    const uint32_t n = lv_obj_get_child_count(gmScr);
    for (uint32_t c = 0; c < n; c++) {
      lv_obj_t *o = lv_obj_get_child(gmScr, c);
      lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
      const uint32_t avg = live(&mx);
      lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
      Serial.printf("[BENCH] g-meter live without #%lu (%dx%d): %lu avg, %lu max\n",
                    (unsigned long)c, (int)lv_obj_get_width(o),
                    (int)lv_obj_get_height(o), (unsigned long)avg,
                    (unsigned long)mx);
    }
  }

  // Boost as it changes in the car: a new sample every 2 request slots, a
  // frame every 16 ms in between (so the arc glide is exercised), with small
  // steps per sample (0.01 bar) and big ones (0.06). Rim off, as in the car.
  if (ui_screen(VIEW_BOOST)) {
    ui_set_view(VIEW_BOOST);
    lv_obj_add_flag(noticeRing, LV_OBJ_FLAG_HIDDEN);
    static const float stepBar[] = { 0.01f, 0.06f };
    const uint32_t samplePeriod = BOOST_SAMPLE_MS;
    // Pass 0: everything; 1: number hidden; 2: arc hidden.
    lv_obj_t *const hide[] = { NULL, ui_boost_value(), ui_boost_arc() };
    static const char *const hideName[] = { "", ", number hidden", ", arc hidden" };
    for (int h = 0; h < 3; h++) {
      if (hide[h]) lv_obj_add_flag(hide[h], LV_OBJ_FLAG_HIDDEN);
      for (int s = 0; s < 2; s++) {
        benchRefr(NULL);
        uint32_t sum = 0, mx = 0;
        const int N = 90;                         // ~1.5 s of frames
        const uint32_t start = millis();
        for (int k = 0; k < N; k++) {
          const uint32_t sample = (millis() - start) / samplePeriod;
          ui_update_boost(true, 0.3f + stepBar[s] * sample);
          const uint32_t t0 = micros();
          lv_refr_now(NULL);
          const uint32_t us = micros() - t0;
          sum += us; if (us > mx) mx = us;
          delay(16);
        }
        Serial.printf("[BENCH] boost live, %.2f bar/frame%s: %lu us/frame avg, %lu max\n",
                      stepBar[s], hideName[h], (unsigned long)(sum / N),
                      (unsigned long)mx);
        esp_task_wdt_reset();
      }
      if (hide[h]) lv_obj_clear_flag(hide[h], LV_OBJ_FLAG_HIDDEN);
    }
  }

  // Which objects are expensive: hide each child of the heavier screens in
  // turn (rim still off) and see what a full redraw saves.
  static const GaugeView heavy[] = { VIEW_GMETER, VIEW_OIL, VIEW_BOOST };
  for (unsigned h = 0; h < sizeof(heavy) / sizeof(heavy[0]); h++) {
    lv_obj_t *const hs = ui_screen(heavy[h]);
    if (!hs) continue;
    ui_set_view(heavy[h]);
    benchRefr(NULL);
    const uint32_t base = (benchRefr(NULL) + benchRefr(NULL)) / 2;
    Serial.printf("[BENCH] view %d per object (base %lu us):\n",
                  (int)heavy[h], (unsigned long)base);
    const uint32_t n = lv_obj_get_child_count(hs);
    for (uint32_t c = 0; c < n; c++) {
      lv_obj_t *o = lv_obj_get_child(hs, c);
      if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) continue;
      lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
      const uint32_t t = (benchRefr(NULL) + benchRefr(NULL)) / 2;
      lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
      const char *kind = lv_obj_check_type(o, &lv_arc_class)   ? "arc"
                       : lv_obj_check_type(o, &lv_line_class)  ? "line"
                       : lv_obj_check_type(o, &lv_label_class) ? "label"
                       : "obj";
      Serial.printf("[BENCH]   #%lu %s %dx%d: saves %ld us\n", (unsigned long)c, kind,
                    (int)lv_obj_get_width(o), (int)lv_obj_get_height(o),
                    (long)base - (long)t);
      esp_task_wdt_reset();
    }
  }

  if (ringWasHidden) lv_obj_add_flag(noticeRing, LV_OBJ_FLAG_HIDDEN);
  else               lv_obj_clear_flag(noticeRing, LV_OBJ_FLAG_HIDDEN);
  ui_set_view(was);
}
#endif
