// Stage 2 — hardware bring-up for the Waveshare ESP32-S3-Touch-AMOLED-1.8.
//
// Not the buddy yet. This is the "does the board actually work" sketch:
//   1. SH8601 AMOLED comes up, the PSRAM canvas flushes  -> "hello buddy"
//   2. QMI8658 IMU answers on I2C                         -> live accel xyz
//   3. FT3168 touch answers on I2C                        -> tap coords
// Everything is also echoed to USB-CDC serial so you can verify headless.
//
// Build/flash (on a real machine — cannot be done from this environment):
//   pio run -e ws-amoled-18 -t upload && pio device monitor
//
// Once this is confirmed on-device, stage 3 ports main.cpp onto the same HAL.

#include <Arduino.h>
#include <Wire.h>
#include "board_pins.h"
#include "hal/display.h"

#include <SensorQMI8658.hpp>

// RGB565 colors (same encoding as TFT_eSPI).
static const uint16_t C_BG    = 0x0000;
static const uint16_t C_TEXT  = 0xFFFF;
static const uint16_t C_BODY  = 0x6B0D;  // olive-ish, bufo green vibe
static const uint16_t C_DIM   = 0x8410;
static const uint16_t C_OK    = 0x07E0;
static const uint16_t C_HOT   = 0xFA20;

static Surface       gfx;
static SensorQMI8658 imu;
static bool          imuOk   = false;
static bool          touchOk = false;
static bool          dispOk  = false;   // gate all canvas draws on this

// --- FT3168 touch: minimal direct read (FT6x36-class register layout) ----
// Reg 0x02 = number of touch points; 0x03.. = point 0 (xh,xl,yh,yl).
static bool ft3168Probe() {
  Wire.beginTransmission(TOUCH_ADDR);
  return Wire.endTransmission() == 0;
}

static bool ft3168Read(uint16_t& x, uint16_t& y) {
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)TOUCH_ADDR, 5) != 5) return false;
  uint8_t n  = Wire.read();          // 0x02 touch count
  uint8_t xh = Wire.read();          // 0x03
  uint8_t xl = Wire.read();          // 0x04
  uint8_t yh = Wire.read();          // 0x05
  uint8_t yl = Wire.read();          // 0x06
  if ((n & 0x0F) == 0) return false;
  x = ((uint16_t)(xh & 0x0F) << 8) | xl;
  y = ((uint16_t)(yh & 0x0F) << 8) | yl;
  return true;
}

static void banner() {
  if (!dispOk) return;          // no canvas → don't touch it (would crash)
  gfx.fillSprite(C_BG);

  gfx.setTextDatum(MC_DATUM);
  gfx.setTextSize(4);
  gfx.setTextColor(C_BODY, C_BG);
  gfx.drawString("hello buddy", LCD_WIDTH / 2, 110);

  gfx.setTextSize(2);
  gfx.setTextColor(C_TEXT, C_BG);
  gfx.drawString("AMOLED 368x448", LCD_WIDTH / 2, 160);

  gfx.setTextSize(1);
  gfx.setTextColor(C_DIM, C_BG);
  gfx.drawString("SH8601 + FT3168 + QMI8658", LCD_WIDTH / 2, 188);
  gfx.setTextDatum(TL_DATUM);

  // status lines (updated each loop in the lower area)
  gfx.pushSprite();
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[bringup] ESP32-S3-Touch-AMOLED-1.8");

  // Display first so failures of the rest are still visible on screen.
  if (!gfx.begin()) {
    Serial.println("[bringup] display begin() FAILED");
  } else {
    Serial.println("[bringup] display OK");
  }
  banner();

  // One shared I2C bus for touch + IMU + RTC + PMU.
  Wire.begin(IIC_SDA, IIC_SCL, 400000);

  // QMI8658 IMU.
  imuOk = imu.begin(Wire, IMU_ADDR, IIC_SDA, IIC_SCL);
  if (imuOk) {
    imu.configAccelerometer(SensorQMI8658::ACC_RANGE_4G,
                            SensorQMI8658::ACC_ODR_1000Hz);
    imu.configGyroscope(SensorQMI8658::GYR_RANGE_256DPS,
                        SensorQMI8658::GYR_ODR_896_8Hz);
    imu.enableAccelerometer();
    imu.enableGyroscope();
    Serial.println("[bringup] QMI8658 OK");
  } else {
    Serial.println("[bringup] QMI8658 NOT found at 0x6B");
  }

  // FT3168 touch.
  touchOk = ft3168Probe();
  pinMode(TP_INT, INPUT);
  Serial.printf("[bringup] FT3168 %s at 0x38\n", touchOk ? "OK" : "NOT found");
}

void loop() {
  static uint32_t lastLog = 0;
  float ax = 0, ay = 0, az = 0;
  if (imuOk && imu.getDataReady()) imu.getAccelerometer(ax, ay, az);

  uint16_t tx = 0, ty = 0;
  bool touched = touchOk && ft3168Read(tx, ty);

  // Repaint the status band ~10 fps.
  static uint32_t lastPaint = 0;
  if (dispOk && millis() - lastPaint >= 100) {
    lastPaint = millis();
    gfx.fillRect(0, 230, LCD_WIDTH, LCD_HEIGHT - 230, C_BG);

    int y = 244;
    auto line = [&](uint16_t col, const char* fmt, ...) {
      char b[64]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
      gfx.setTextColor(col, C_BG);
      gfx.setTextSize(2);
      gfx.setCursor(16, y);
      gfx.print(b);
      y += 26;
    };

    line(imuOk ? C_OK : C_HOT, "IMU  %s", imuOk ? "ok" : "--");
    if (imuOk) line(C_DIM, " x%+.2f y%+.2f z%+.2f", ax, ay, az);
    line(touchOk ? C_OK : C_HOT, "TOUCH %s", touchOk ? "ok" : "--");
    if (touched) {
      line(C_TEXT, " tap %u,%u", tx, ty);
      // little crosshair where you touched
      gfx.drawCircle(tx, ty, 12, C_HOT);
      gfx.drawFastHLine(tx - 18, ty, 36, C_HOT);
      gfx.drawFastVLine(tx, ty - 18, 36, C_HOT);
    } else {
      line(C_DIM, " (touch the screen)");
    }
    gfx.pushSprite();
  }

  if (millis() - lastLog >= 1000) {
    lastLog = millis();
    Serial.printf("[bringup] imu=%d a(%.2f,%.2f,%.2f) touch=%d (%u,%u)\n",
                  imuOk, ax, ay, az, touched, tx, ty);
  }

  delay(16);
}
