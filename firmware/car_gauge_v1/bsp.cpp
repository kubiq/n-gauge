// ============================================================================
// bsp.cpp  —  Waveshare ESP32-S3-Touch-AMOLED-1.75 board glue.
//   Panel/touch/IMU pin values and the CO5300 init come from Waveshare's own
//   Arduino examples for this exact board (examples/arduino/01_HelloWorld and
//   06_LVGL_Widgets in waveshareteam/ESP32-S3-Touch-AMOLED-1.75).
//   LVGL v9 display registration follows Arduino_GFX's LVGL_Arduino_v9 example.
// ============================================================================
#include "bsp.h"
#include "config.h"
#include "settings.h"
#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <SensorQMI8658.hpp>
#include <touch/TouchDrvCST92xx.h>
#include "FS.h"
#include "SD_MMC.h"      // bsp_prepare_sleep unmounts the card
#include "lvgl.h"

// ---- Board pins (Waveshare pin_config.h for this board) --------------------
#define LCD_SDIO0   4
#define LCD_SDIO1   5
#define LCD_SDIO2   6
#define LCD_SDIO3   7
#define LCD_SCLK   38
#define LCD_CS     12
#define LCD_RESET  39
#define LCD_W     466
#define LCD_H     466
#define IIC_SDA    15
#define IIC_SCL    14
#define I2C_FREQ_HZ 400000
#define TP_INT     11
#define TP_RESET   40
// NS4150B speaker amplifier enable. No speaker is fitted and the audio code is
// gone, but the pin still has to be held low - see bsp_init().
#define AUDIO_PA_GPIO 46

static Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

// The trailing 6,0,0,0 are this panel's column/row offsets - the 466x466
// active area starts 6 pixels in. Wrong offsets show as a shifted image.
static Arduino_CO5300 *gfx = new Arduino_CO5300(
    bus, LCD_RESET, 0 /*rotation*/, LCD_W, LCD_H, 6, 0, 0, 0);

static SensorQMI8658 imu;
static bool imuOk = false;

static TouchDrvCST92xx touch;
static bool touchOk = false;

void bsp_init() {
  pinMode(BTN_NEXT_GPIO, BTN_NEXT_ACTIVE_LOW ? INPUT_PULLUP : INPUT_PULLDOWN);

  // Hold the speaker amplifier disabled, explicitly.
  //
  // The NS4150B's enable is GPIO46. Removing the audio subsystem deleted the
  // code that drove it - both the HIGH that enabled it and the LOW that shut
  // it down for sleep - so the pin was left as a reset-default input with no
  // pull, i.e. floating. A floating enable on an amplifier is not "off": it
  // can sit at a level the part reads as enabled, drawing quiescent current
  // continuously, for a speaker that is not even fitted. Driving it low costs
  // one line and removes the ambiguity in deep sleep as well, since an output
  // level is held through sleep while a floating input is anyone's guess.
  pinMode(AUDIO_PA_GPIO, OUTPUT);
  digitalWrite(AUDIO_PA_GPIO, LOW);

  // 400 kHz fast mode, not the 100 kHz default: every chip on this bus is
  // rated for it. At 100 kHz the IMU poll alone (status + 6-byte sample) took
  // ~1.5 ms of every loop pass - the single biggest cost outside LVGL, and
  // enough to make the 16 ms frame timer fire late. The touch read, done by
  // LVGL, is on the same bus and gets the same speed-up.
  Wire.begin(IIC_SDA, IIC_SCL, I2C_FREQ_HZ);

  // Bring-up aid: which I2C devices actually answer. A missing address here
  // localises a fault to the bus in one line, before any driver runs.
  // Expect: 18 20 34 40 51 5A 6B 7E - see the I2C map in bsp.h for what each
  // one is, and why 0x7E is not a chip.
  Serial.print(F("[I2C] devices:"));
  for (uint8_t a = 1; a < 0x7F; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) Serial.printf(" 0x%02X", a);
  }
  Serial.println();

  if (!gfx->begin()) Serial.println(F("[ERR] gfx->begin() failed"));
  gfx->fillScreen(RGB565_BLACK);
  gfx->setBrightness(settings.brightness);
  Serial.println(F("[OK] CO5300 AMOLED up"));

  // The CST9217 sometimes isn't responsive this soon after a hard reset, so
  // retry rather than losing touch for the whole power cycle.
  touch.setPins(TP_RESET, TP_INT);
  for (int attempt = 0; attempt < 3; attempt++) {
    touchOk = touch.begin(Wire, CST92XX_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
    if (touchOk) break;
    delay(60);
  }
  if (touchOk) {
    touch.setMaxCoordinates(LCD_W, LCD_H);
    touch.setMirrorXY(true, true);   // orientation used by Waveshare's own example
    Serial.printf("[OK] touch up (%s)\n", touch.getModelName());
  } else {
    Serial.println(F("[ERR] CST9217 touch not found"));
  }

  imuOk = imu.begin(Wire, QMI8658_L_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (imuOk) {
    imu.configAccelerometer(SensorQMI8658::ACC_RANGE_4G,
                            SensorQMI8658::ACC_ODR_1000Hz,
                            SensorQMI8658::LPF_MODE_0);
    imu.enableAccelerometer();
    Serial.println(F("[OK] QMI8658 IMU up"));
    // The drivers call Wire.begin() themselves; make sure none of them
    // quietly put the bus back to 100 kHz.
    Serial.printf("[I2C] clock %lu Hz\n", (unsigned long)Wire.getClock());
  } else {
    Serial.println(F("[ERR] QMI8658 not found - g-meter will read zero"));
  }
}

// ---- LVGL v9 glue ----------------------------------------------------------
static uint32_t lvglTick() { return millis(); }

#if FRAME_STATS
static struct {
  uint32_t frames;       // refreshes that drew something
  uint32_t idle;         // refreshes with nothing invalidated
  uint32_t refrStart;
  uint32_t refrUs, refrMaxUs;   // whole refresh (render + flush), drawn frames only
  uint32_t flushUs;             // of which pushing pixels
  uint32_t flushes;
  uint64_t px;
  uint32_t loops, loopUs, loopMaxUs, lvUs;
} fst;

static void lvglRefrStart(lv_event_t *) { fst.refrStart = micros(); }
static void lvglRenderReady(lv_event_t *) { fst.frames++; }
static void lvglRefrReady(lv_event_t *) {
  static uint32_t lastFrames = 0;
  if (fst.frames == lastFrames) { fst.idle++; return; }
  lastFrames = fst.frames;
  const uint32_t us = micros() - fst.refrStart;
  fst.refrUs += us;
  if (us > fst.refrMaxUs) fst.refrMaxUs = us;
}

void bsp_frame_stats_loop(uint32_t loopUs, uint32_t lvUs, int view) {
  fst.loops++;
  fst.loopUs += loopUs;
  fst.lvUs   += lvUs;
  if (loopUs > fst.loopMaxUs) fst.loopMaxUs = loopUs;

  static uint32_t since = millis();
  const uint32_t el = millis() - since;
  if (el < 2000) return;
  since += el;
  const uint32_t f = fst.frames ? fst.frames : 1;
  Serial.printf("[FPS] view %d: %.1f fps (%lu idle) | refr avg %lu max %lu us | "
                "flush %lu us/frame, %lu KPx/frame, %.0f us/KPx | "
                "loop %.0f Hz avg %lu max %lu us, lvgl %lu us/pass\n",
                view, fst.frames * 1000.0f / el, (unsigned long)fst.idle,
                (unsigned long)(fst.refrUs / f), (unsigned long)fst.refrMaxUs,
                (unsigned long)(fst.flushUs / f), (unsigned long)(fst.px / f / 1000),
                fst.px ? fst.flushUs * 1000.0f / fst.px : 0.0f,
                fst.loops * 1000.0f / el,
                (unsigned long)(fst.loopUs / fst.loops), (unsigned long)fst.loopMaxUs,
                (unsigned long)(fst.lvUs / fst.loops));
  memset(&fst, 0, sizeof(fst));
}
#endif

static void lvglFlush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
#if FRAME_STATS
  const uint32_t t0 = micros();
#endif
  gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)px_map,
                          lv_area_get_width(area), lv_area_get_height(area));
#if FRAME_STATS
  fst.flushUs += micros() - t0;
  fst.flushes++;
  fst.px += (uint32_t)lv_area_get_width(area) * lv_area_get_height(area);
#endif
  lv_display_flush_ready(disp);
}

// The CO5300 addresses pixels in 2-pixel units. CASET/PASET get our coordinates
// verbatim (see Arduino_CO5300::writeAddrWindow), so an odd start or even end
// gets snapped by the controller - the pixels we then stream no longer match
// the real row width and the whole image shears diagonally. Snapping the area
// to even start / odd end keeps width and height even so nothing is snapped.
// LVGL v8 did this with disp_drv.rounder_cb; v9 uses this event instead.
static void lvglTouchRead(lv_indev_t *indev, lv_indev_data_t *data) {
  (void)indev;
  if (!touchOk) { data->state = LV_INDEV_STATE_RELEASED; return; }

  int16_t tx[1], ty[1];
  if (touch.getPoint(tx, ty, 1) > 0) {     // this does the I2C read
    data->point.x = tx[0];
    data->point.y = ty[0];
    data->state   = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

static void lvglAlignArea(lv_event_t *e) {
  lv_area_t *area = (lv_area_t *)lv_event_get_param(e);
  area->x1 &= ~1;
  area->y1 &= ~1;
  area->x2 |= 1;   // 466 px -> max index 465, already odd, so this can't overflow
  area->y2 |= 1;
}

void bsp_lvgl_init() {
  lv_init();
  lv_tick_set_cb(lvglTick);

  // Partial-render strip. A full 466x466 frame would be 434 KB, which won't
  // fit in DMA-capable internal RAM, so render 40 lines at a time. Keep the
  // line count EVEN so each split strip stays 2-pixel aligned (see
  // lvglAlignArea) - an odd strip height would re-introduce the shear.
  const uint32_t bufPx = LCD_W * 40;
  void *buf = heap_caps_malloc(bufPx * 2, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
  if (!buf) buf = heap_caps_malloc(bufPx * 2, MALLOC_CAP_8BIT);
  if (!buf) {
    Serial.println(F("[ERR] LVGL draw buffer alloc failed - no UI"));
    return;
  }

  lv_display_t *disp = lv_display_create(LCD_W, LCD_H);
  lv_display_set_flush_cb(disp, lvglFlush);
  lv_display_add_event_cb(disp, lvglAlignArea, LV_EVENT_INVALIDATE_AREA, NULL);
#if FRAME_STATS
  lv_display_add_event_cb(disp, lvglRefrStart,   LV_EVENT_REFR_START,   NULL);
  lv_display_add_event_cb(disp, lvglRenderReady, LV_EVENT_RENDER_READY, NULL);
  lv_display_add_event_cb(disp, lvglRefrReady,   LV_EVENT_REFR_READY,   NULL);
#endif
  lv_display_set_buffers(disp, buf, NULL, bufPx * 2, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_timer_set_period(lv_display_get_refr_timer(disp), UI_FRAME_MS);
  Serial.println(F("[OK] LVGL display registered"));

  if (touchOk) {
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, lvglTouchRead);
    Serial.println(F("[OK] LVGL touch input registered"));
  }
}

void bsp_display_off() {
  gfx->setBrightness(0);
  gfx->displayOff();
}

// Put every peripheral we can into its lowest state before deep sleep.
//
// This exists because of a measurement: the board alone, "asleep", drew 15 mA
// at 5 V - against an ESP32-S3 deep-sleep current of about 20 uA. Deep sleep
// stops the CPU, it does not switch off anything hanging off the I2C bus or
// the SD interface, and all of that stays powered from rails the AXP2101 keeps
// up. So the chip slept and the board did not.
//
// Everything here is re-initialised by bsp_init() on the next boot, because a
// wake is a full cold start - there is no resume path to keep consistent.
void bsp_prepare_sleep() {
  bsp_display_off();

  // The accelerometer was left sampling at 1 kHz, which it happily keeps doing
  // while the CPU is asleep.
  if (imuOk) {
    imu.disableAccelerometer();
    imu.disableGyroscope();
    imu.powerDown();
  }

  // The touch controller scans continuously; nothing is going to read it.
  if (touchOk) touch.sleep();

  // Unmount the card. logger_close() flushes and closes the files, but the
  // card stays selected and clocked until the peripheral is released.
  SD_MMC.end();
}

// Wake-on-motion on the QMI8658's INT2, which is wired straight to GPIO21
// (schematic, verified 2026-10-07). The sensor switches to low-power
// sampling and stops producing data, so this replaces the g-meter until the
// next bsp_init(). Threshold in mg (1 mg steps, 1-255).
bool bsp_imu_wom_arm(uint8_t thresholdMg) {
  if (!imuOk) return false;
  return imu.configWakeOnMotion(thresholdMg) == 0;
}

// The QMI8658's STATUS1: bit 2 is "wake on motion detected". Reading it is
// how the sensor's event latch is cleared.
uint8_t bsp_imu_status1() {
  return imuOk ? (uint8_t)imu.getStatusRegister() : 0;
}

void bsp_display_set_brightness(uint8_t b) {
  gfx->setBrightness(b);
}

// Returns false when there is no fresh sample, and hands back the last good
// one rather than zeros.
//
// This is called twice per loop - once for the g-meter, once for the CSV - and
// the QMI8658 produces one sample per millisecond, so the second caller
// routinely found nothing new. Returning 0,0,0 then wrote a plausible-looking
// "0.000,0.000,0.000" into the very columns added so the mounting angle could
// be derived offline, and snapped the g-meter dot to dead centre at the
// minimum size. A logged zero is indistinguishable from a real reading, which
// is the rule the rest of the logger already follows by emitting empty fields.
bool bsp_imu_read(float *gx, float *gy, float *gz) {
  static float lastX = 0.0f, lastY = 0.0f, lastZ = 0.0f;
  static bool  haveSample = false;

  if (imuOk && imu.getDataReady()) {
    float x, y, z;
    if (imu.getAccelerometer(x, y, z)) {
      lastX = x; lastY = y; lastZ = z;
      haveSample = true;
      *gx = x; *gy = y; *gz = z;
      return true;
    }
  }
  *gx = lastX; *gy = lastY; *gz = lastZ;
  return haveSample;        // false only before the very first good sample
}

