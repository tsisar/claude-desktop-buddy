// Stage 2 — hardware bring-up / display diagnostic for the
// Waveshare ESP32-S3-Touch-AMOLED-1.8.
//
// Black screen debugging: this version isolates *where* the display path
// breaks by testing two independent routes and logging status every second
// (so you can read it any time, not just catch the boot banner):
//
//   A. DIRECT panel test  — panel()->fillScreen(R/G/B), bypassing the
//      offscreen canvas. Proves the SH8601 glass + QSPI + brightness work.
//   B. CANVAS test        — fillSprite()+pushSprite(), the path the real
//      firmware uses. If A lights up but B is black, the canvas/flush is
//      the problem; if both are black, it's panel power/init.
//
// Build/flash:  pio run -e ws-amoled-18 -t upload && pio device monitor

#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include "board_pins.h"
#include "hal/display.h"

#include <SensorQMI8658.hpp>
// NOTE: no AXP2101 power-init here. On this board the PMU already powers the
// AMOLED rails at reset (rail dump showed ALDO1/ALDO3/BLDO1/BLDO2 all ON),
// and adding powerInit() before the display only turned the screen black.
// The display works with just gfx.begin(), exactly like Waveshare's
// 01_HelloWorld. Kept out of the bring-up path on purpose.

// RGB565 colors.
static const uint16_t C_BG    = 0x0000;
static const uint16_t C_TEXT  = 0xFFFF;
static const uint16_t C_BODY  = 0x6B0D;
static const uint16_t C_DIM   = 0x8410;
static const uint16_t C_OK    = 0x07E0;
static const uint16_t C_HOT   = 0xFA20;

static Surface       gfx;
static SensorQMI8658 imu;
static bool          imuOk   = false;
static bool          touchOk = false;
static bool          dispOk  = false;

// --- FT3168 touch: minimal direct read (FT6x36-class register layout) ----
static bool ft3168Probe() {
  Wire.beginTransmission(TOUCH_ADDR);
  return Wire.endTransmission() == 0;
}

static bool ft3168Read(uint16_t& x, uint16_t& y) {
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)TOUCH_ADDR, 5) != 5) return false;
  uint8_t n  = Wire.read();
  uint8_t xh = Wire.read();
  uint8_t xl = Wire.read();
  uint8_t yh = Wire.read();
  uint8_t yl = Wire.read();
  if ((n & 0x0F) == 0) return false;
  x = ((uint16_t)(xh & 0x0F) << 8) | xl;
  y = ((uint16_t)(yh & 0x0F) << 8) | yl;
  return true;
}

// Draw straight to the glass, no canvas in the path.
static void directFill(uint16_t color) {
  Arduino_SH8601* p = gfx.panel();
  if (p) p->fillScreen(color);
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  delay(400);
  Serial.println("\n[bringup] ESP32-S3-Touch-AMOLED-1.8 diag boot");

  // Display first (no PMU touching). This is the exact ordering that showed
  // colours on hardware.
  dispOk = gfx.begin();
  Serial.printf("[bringup] display %s psram=%u free=%u heap=%u\n",
                dispOk ? "OK" : "FAILED",
                (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram(),
                (unsigned)ESP.getFreeHeap());

  if (dispOk) {
    // Route A — direct to panel. Force brightness up first in case the
    // default is 0. If the glass works at all, you'll see R, G, B flashes.
    gfx.setBrightness(255);
    Serial.println("[bringup] direct panel: RED");   directFill(0xF800); delay(700);
    Serial.println("[bringup] direct panel: GREEN"); directFill(0x07E0); delay(700);
    Serial.println("[bringup] direct panel: BLUE");  directFill(0x001F); delay(700);

    // Route B — through the canvas (what the firmware uses).
    Serial.println("[bringup] canvas: fill+text");
    gfx.fillSprite(C_BG);
    gfx.setTextDatum(MC_DATUM);
    gfx.setTextSize(4); gfx.setTextColor(C_BODY, C_BG);
    gfx.drawString("hello buddy", LCD_WIDTH / 2, 180);
    gfx.setTextSize(2); gfx.setTextColor(C_TEXT, C_BG);
    gfx.drawString("canvas path", LCD_WIDTH / 2, 230);
    gfx.setTextDatum(TL_DATUM);
    gfx.pushSprite();
  }

  // Shared I2C for touch + IMU (after the display, like the working build).
  Wire.begin(IIC_SDA, IIC_SCL, 400000);
  imuOk = imu.begin(Wire, IMU_ADDR, IIC_SDA, IIC_SCL);
  if (imuOk) {
    imu.configAccelerometer(SensorQMI8658::ACC_RANGE_4G,
                            SensorQMI8658::ACC_ODR_1000Hz);
    imu.enableAccelerometer();
  }
  Serial.printf("[bringup] QMI8658 %s\n", imuOk ? "OK" : "NOT found");

  touchOk = ft3168Probe();
  pinMode(TP_INT, INPUT);
  Serial.printf("[bringup] FT3168 %s at 0x38\n", touchOk ? "OK" : "NOT found");
  Serial.println("[bringup] setup done");
}

void loop() {
  static uint32_t lastLog = 0, lastPaint = 0;
  static uint8_t  phase = 0;

  float ax = 0, ay = 0, az = 0;
  if (imuOk && imu.getDataReady()) imu.getAccelerometer(ax, ay, az);
  uint16_t tx = 0, ty = 0;
  bool touched = touchOk && ft3168Read(tx, ty);

  // Alternate the two display paths every 1.5s so the screen itself tells
  // us where the break is, no serial needed:
  //   phases 0,1,2 = DIRECT panel fill  R, G, B   (bypasses canvas)
  //   phases 3,4,5 = CANVAS fill        R, G, B   (the firmware's path)
  // Whichever set lights up is the working route.
  if (dispOk && millis() - lastPaint >= 1500) {
    lastPaint = millis();
    const uint16_t rgb[] = { 0xF800, 0x07E0, 0x001F };  // R G B
    uint8_t p = phase % 6;
    if (p < 3) {
      directFill(rgb[p]);                 // route A: straight to glass
      Serial.printf("[bringup] DIRECT panel %c\n", "RGB"[p]);
    } else {
      gfx.fillSprite(rgb[p - 3]);          // route B: canvas -> flush
      gfx.setTextDatum(TL_DATUM);
      gfx.setTextSize(3); gfx.setTextColor(0xFFFF, rgb[p - 3]);
      gfx.setCursor(12, 12); gfx.print("CANVAS");
      gfx.pushSprite();
      Serial.printf("[bringup] CANVAS %c\n", "RGB"[p - 3]);
    }
    phase++;
  }

  if (millis() - lastLog >= 1000) {
    lastLog = millis();
    Serial.printf("[bringup] disp=%d psram=%u heap=%u | imu=%d a(%.2f,%.2f,%.2f) | touch=%d (%u,%u)\n",
                  dispOk, (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreeHeap(),
                  imuOk, ax, ay, az, touched, tx, ty);
  }
  delay(16);
}
