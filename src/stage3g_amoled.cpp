// Stage 3g — hardware-verification harness for the new HAL modules:
// QMI8658 IMU (shake / face-down), PCF85063 RTC (clock face source), and
// AXP2101 PWRON button (screen-toggle / power-off intent). These three
// closed the M5-side hardware features that stage 3f's BLE-driven runtime
// couldn't reach. Stage 3h folds them into main.cpp.
//
// Layout: top half = IMU (live accel x/y/z, shake/face-down flags),
// mid    = RTC (HH:MM:SS, date, week-day, "set?" if invalid),
// bottom = PWRON event log (last 4 events with millis() timestamp).
//
// Build switch: enable just this file in build_src_filter for ws-amoled-18,
// disable stage3f (they both define setup()/loop()).

#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include "board_pins.h"
#include "hal/display.h"
#include "hal/power.h"
#include "hal/imu.h"
#include "hal/rtc.h"

static Surface gfx;
static bool    dispOk = false;
static const int W = LCD_WIDTH, H = LCD_HEIGHT;

// ── shake / face-down detectors — same algorithm as main.cpp on M5 ──
static float    accelBaseline = 1.0f;
static int8_t   faceDownFrames = 0;
static uint32_t lastShakeMs = 0;
static bool     shakeFlag = false;
static bool     faceDown  = false;

static void updateShake(float ax, float ay, float az) {
  float mag = sqrtf(ax*ax + ay*ay + az*az);
  float delta = fabsf(mag - accelBaseline);
  accelBaseline = accelBaseline * 0.95f + mag * 0.05f;
  if (delta > 0.8f) { shakeFlag = true; lastShakeMs = millis(); }
  else if (millis() - lastShakeMs > 800) shakeFlag = false;
}

static void updateFaceDown(float ax, float ay, float az) {
  bool down = (az < -0.7f) && fabsf(ax) < 0.4f && fabsf(ay) < 0.4f;
  if (down) { if (faceDownFrames < 20) faceDownFrames++; }
  else      { if (faceDownFrames > -10) faceDownFrames--; }
  if      (!faceDown && faceDownFrames >= 15) faceDown = true;
  else if ( faceDown && faceDownFrames <= -8) faceDown = false;
}

// ── PWRON event log (ring buffer of 4) ──
struct PwEvent { uint32_t ms; PwronEvent kind; };
static PwEvent pwLog[4];
static uint8_t pwHead = 0;
static void logPw(PwronEvent k) {
  pwLog[pwHead] = { millis(), k };
  pwHead = (pwHead + 1) % 4;
}

static const char* pwName(PwronEvent k) {
  switch (k) {
    case PWRON_SHORT: return "SHORT";
    case PWRON_LONG:  return "LONG";
    default:          return "----";
  }
}

void setup() {
  Serial.begin(115200); Serial.setTxTimeoutMs(0); delay(300);
  Serial.println("\n[stage3g] HAL verification harness");

  Wire.begin(IIC_SDA, IIC_SCL, 400000);
  powerInit(Wire);     // enable panel rails + PWRON IRQ
  dispOk = gfx.begin();
  imuInit(Wire);
  rtcInit(Wire);

  Serial.printf("[stage3g] disp=%d imu=%d rtc=%d pmu=%d\n",
                dispOk, imuOk(), rtcOk(), powerOk());
}

void loop() {
  static uint32_t nextDraw = 0;
  uint32_t now = millis();

  // Pump sensors at ~50 Hz so the shake detector matches the M5 cadence.
  static uint32_t nextSensor = 0;
  if (now >= nextSensor) {
    nextSensor = now + 20;
    float ax = 0, ay = 0, az = 0;
    if (imuGetAccel(&ax, &ay, &az)) {
      updateShake(ax, ay, az);
      updateFaceDown(ax, ay, az);
    }
  }

  // PWRON events: poll once per loop, log everything non-NONE.
  PwronEvent pw = powerPollButton();
  if (pw != PWRON_NONE) {
    logPw(pw);
    Serial.printf("[stage3g] PWRON %s\n", pwName(pw));
  }

  if (!dispOk || now < nextDraw) { delay(8); return; }
  nextDraw = now + 200;

  gfx.fillSprite(0x0000);
  gfx.setTextDatum(TL_DATUM);

  // ── IMU panel ──
  gfx.setTextSize(3);
  gfx.setTextColor(0xFFFF, 0x0000);
  gfx.setCursor(10, 10);  gfx.print("IMU");

  float ax = 0, ay = 0, az = 0;
  imuGetAccel(&ax, &ay, &az);
  gfx.setTextSize(2);
  gfx.setTextColor(0xC618, 0x0000);
  gfx.setCursor(10, 50);  gfx.printf("x % .2f g", ax);
  gfx.setCursor(10, 75);  gfx.printf("y % .2f g", ay);
  gfx.setCursor(10, 100); gfx.printf("z % .2f g", az);

  gfx.setTextSize(2);
  gfx.setTextColor(shakeFlag ? 0xFFE0 : 0x4208, 0x0000);
  gfx.setCursor(W/2 + 10, 50); gfx.print(shakeFlag ? "SHAKE" : "shake");
  gfx.setTextColor(faceDown  ? 0x07E0 : 0x4208, 0x0000);
  gfx.setCursor(W/2 + 10, 75); gfx.print(faceDown  ? "FACEDN" : "facedn");
  gfx.setTextColor(0xC618, 0x0000);
  gfx.setCursor(W/2 + 10, 100); gfx.printf("%.1fC", imuTemp());

  gfx.drawFastHLine(0, 140, W, 0x4208);

  // ── RTC panel ──
  gfx.setTextSize(3);
  gfx.setTextColor(0xFFFF, 0x0000);
  gfx.setCursor(10, 155); gfx.print("RTC");

  RtcTime t; RtcDate d;
  bool tOk = rtcGetTime(&t), dOk = rtcGetDate(&d);
  gfx.setTextSize(4);
  if (tOk) {
    gfx.setTextColor(0x07FF, 0x0000);
    gfx.setCursor(10, 200); gfx.printf("%02u:%02u:%02u",
                                       t.Hours, t.Minutes, t.Seconds);
  } else {
    gfx.setTextColor(0xFA20, 0x0000);
    gfx.setCursor(10, 200); gfx.print("--:--:--");
  }
  gfx.setTextSize(2);
  gfx.setTextColor(0xC618, 0x0000);
  if (dOk) {
    gfx.setCursor(10, 250);
    gfx.printf("%04u-%02u-%02u  w=%u", d.Year, d.Month, d.Date, d.WeekDay);
  } else {
    gfx.setCursor(10, 250); gfx.print("set time via bridge");
  }

  gfx.drawFastHLine(0, 290, W, 0x4208);

  // ── PWRON event log ──
  gfx.setTextSize(3);
  gfx.setTextColor(0xFFFF, 0x0000);
  gfx.setCursor(10, 305); gfx.print("PWRON");

  gfx.setTextSize(2);
  // Render oldest-first so newest sits at the bottom.
  for (int i = 0; i < 4; i++) {
    int idx = (pwHead + i) % 4;
    int y = 345 + i * 22;
    if (pwLog[idx].kind == PWRON_NONE) {
      gfx.setTextColor(0x4208, 0x0000);
      gfx.setCursor(10, y); gfx.print("- ---- ----");
    } else {
      bool newest = (i == 3);
      gfx.setTextColor(newest ? 0xFFE0 : 0xC618, 0x0000);
      gfx.setCursor(10, y);
      uint32_t s = pwLog[idx].ms / 1000;
      gfx.printf("%lus  %s", (unsigned long)s, pwName(pwLog[idx].kind));
    }
  }

  gfx.pushSprite();
  delay(8);
}