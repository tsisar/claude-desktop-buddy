// Stage 3a — full-screen buddy layout proof on the AMOLED.
//
// NOT the real firmware yet. Goal: confirm the full-screen 368x448 layout
// looks right before porting main.cpp's state machine:
//   - one ASCII buddy (cat, idle pose) drawn LARGE, centered in the upper area
//   - two big touch targets at the bottom: APPROVE (green) / DENY (red)
//   - tapping a button flashes it + logs to serial
//
// Reuses the verified display + FT3168 touch from the bring-up. No BLE, no
// states, no IMU — just geometry + touch ergonomics on the big screen.
//
// Build: this is selected by build_src_filter in [env:ws-amoled-18].

#include <Arduino.h>
#include <Wire.h>
#include "board_pins.h"
#include "hal/display.h"

// RGB565
static const uint16_t C_BG    = 0x0000;
static const uint16_t C_BODY  = 0x6B0D;  // bufo green
static const uint16_t C_TEXT  = 0xFFFF;
static const uint16_t C_DIM   = 0x8410;
static const uint16_t C_GREEN = 0x07E0;
static const uint16_t C_RED   = 0xF800;
static const uint16_t C_DGREEN= 0x02E0;  // dim green (button idle)
static const uint16_t C_DRED  = 0x7800;  // dim red (button idle)

static Surface gfx;
static bool    dispOk  = false;
static bool    touchOk = false;

// --- FT3168 touch (FT6x36-class) -----------------------------------------
static bool ft3168Probe() {
  Wire.beginTransmission(TOUCH_ADDR);
  return Wire.endTransmission() == 0;
}
static bool ft3168Read(uint16_t& x, uint16_t& y) {
  Wire.beginTransmission(TOUCH_ADDR);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)TOUCH_ADDR, 5) != 5) return false;
  uint8_t n = Wire.read(), xh = Wire.read(), xl = Wire.read(),
          yh = Wire.read(), yl = Wire.read();
  if ((n & 0x0F) == 0) return false;
  x = ((uint16_t)(xh & 0x0F) << 8) | xl;
  y = ((uint16_t)(yh & 0x0F) << 8) | yl;
  return true;
}

// --- layout ---------------------------------------------------------------
static const int W = LCD_WIDTH;    // 368
static const int H = LCD_HEIGHT;   // 448
static const int BTN_H   = 96;             // button band height
static const int BTN_TOP = H - BTN_H;      // 352
static const int BTN_GAP = 8;
static const int BTN_W   = (W - 3 * BTN_GAP) / 2;   // each button width
static const int APPROVE_X = BTN_GAP;
static const int DENY_X    = BTN_GAP * 2 + BTN_W;

// Cat idle pose (REST) from src/buddies/cat.cpp, 12 cols wide.
static const char* const CAT[5] = {
  "            ",
  "   /\\_/\\    ",
  "  ( o   o ) ",
  "  (  w   )  ",
  "  (\")_(\")   ",
};

static void drawButtons(int pressed /* 0=none, 1=approve, 2=deny */) {
  // APPROVE
  gfx.fillRoundRect(APPROVE_X, BTN_TOP, BTN_W, BTN_H, 12,
                    pressed == 1 ? C_GREEN : C_DGREEN);
  gfx.drawRoundRect(APPROVE_X, BTN_TOP, BTN_W, BTN_H, 12, C_GREEN);
  // DENY
  gfx.fillRoundRect(DENY_X, BTN_TOP, BTN_W, BTN_H, 12,
                    pressed == 2 ? C_RED : C_DRED);
  gfx.drawRoundRect(DENY_X, BTN_TOP, BTN_W, BTN_H, 12, C_RED);

  gfx.setTextDatum(MC_DATUM);
  gfx.setTextSize(3);
  gfx.setTextColor(C_TEXT, pressed == 1 ? C_GREEN : C_DGREEN);
  gfx.drawString("APPROVE", APPROVE_X + BTN_W / 2, BTN_TOP + BTN_H / 2);
  gfx.setTextColor(C_TEXT, pressed == 2 ? C_RED : C_DRED);
  gfx.drawString("DENY", DENY_X + BTN_W / 2, BTN_TOP + BTN_H / 2);
  gfx.setTextDatum(TL_DATUM);
}

static void drawScene(int pressed) {
  gfx.fillSprite(C_BG);

  // Buddy: 5 lines of 12-char art at text size 4 (glyph 24px tall, 24px wide
  // → 12 chars = 288px, fits 368 with margin). Centered horizontally, sitting
  // in the upper ~60% above the button band.
  gfx.setTextSize(4);
  gfx.setTextColor(C_BODY, C_BG);
  gfx.setTextDatum(TC_DATUM);
  const int lineH = 8 * 4;           // 32px per glyph row at size 4
  const int blockH = 5 * lineH;
  int y = (BTN_TOP - blockH) / 2;    // vertically center in the area above buttons
  for (int i = 0; i < 5; i++) {
    gfx.drawString(CAT[i], W / 2, y + i * lineH);
  }

  // little name caption under the buddy
  gfx.setTextSize(2);
  gfx.setTextColor(C_DIM, C_BG);
  gfx.drawString("Buddy", W / 2, y + blockH + 12);
  gfx.setTextDatum(TL_DATUM);

  drawButtons(pressed);
  gfx.pushSprite();
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  delay(300);
  Serial.println("\n[stage3a] full-screen buddy layout");

  dispOk = gfx.begin();
  Serial.printf("[stage3a] display %s\n", dispOk ? "OK" : "FAILED");

  Wire.begin(IIC_SDA, IIC_SCL, 400000);
  touchOk = ft3168Probe();
  pinMode(TP_INT, INPUT);
  Serial.printf("[stage3a] touch %s\n", touchOk ? "OK" : "NOT found");

  if (dispOk) drawScene(0);
}

void loop() {
  static int  lastPressed = 0;
  static uint32_t flashUntil = 0;

  uint16_t tx = 0, ty = 0;
  int pressed = 0;
  if (touchOk && ft3168Read(tx, ty)) {
    if (ty >= BTN_TOP) {
      if (tx >= APPROVE_X && tx < APPROVE_X + BTN_W)      pressed = 1;
      else if (tx >= DENY_X && tx < DENY_X + BTN_W)       pressed = 2;
    }
    if (pressed && pressed != lastPressed) {
      Serial.printf("[stage3a] %s tapped (%u,%u)\n",
                    pressed == 1 ? "APPROVE" : "DENY", tx, ty);
      flashUntil = millis() + 250;
    }
  }

  // Hold the flash highlight briefly so a quick tap is visible.
  int show = (millis() < flashUntil) ? lastPressed ? lastPressed : pressed : 0;
  if (pressed) { lastPressed = pressed; show = pressed; }

  static int drawn = -1;
  if (dispOk && show != drawn) {
    drawButtons(show);
    gfx.pushSprite();
    drawn = show;
  }
  if (!pressed && millis() >= flashUntil) lastPressed = 0;

  delay(16);
}
