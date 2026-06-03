#include "expander.h"
#include <Arduino.h>
#include <Wire.h>

// XCA9554 GPIO expander — see expander.h for the why.

static constexpr uint8_t XCA_ADDR    = 0x20;
static constexpr uint8_t REG_OUTPUT  = 0x01;
static constexpr uint8_t REG_CONFIG  = 0x03;

static TwoWire* bus = nullptr;
static bool     ok  = false;

static bool write8(uint8_t reg, uint8_t val) {
  bus->beginTransmission(XCA_ADDR);
  bus->write(reg);
  bus->write(val);
  return bus->endTransmission() == 0;
}

bool expanderInit(TwoWire& w) {
  if (ok) return true;
  bus = &w;
  // Probe — also retries to ride out the post-power-on settle.
  for (int attempt = 0; attempt < 8; attempt++) {
    bus->beginTransmission(XCA_ADDR);
    if (bus->endTransmission() == 0) { ok = true; break; }
    delay(10);
  }
  if (!ok) {
    Serial.printf("[expander] XCA9554 not found at 0x%02X\n", XCA_ADDR);
    return false;
  }
  // Config: pins 0,1,2 as OUTPUT (clear bits 0..2), the rest stays input.
  // Writing 0xF8 = 1111_1000 → bits 0/1/2 low (output), bits 3..7 high.
  if (!write8(REG_CONFIG, 0xF8)) {
    Serial.println("[expander] XCA9554 config write failed");
    ok = false;
    return false;
  }
  Serial.println("[expander] XCA9554 ok");
  return true;
}

bool expanderOk() { return ok; }

void expanderResetPulse() {
  if (!ok) return;
  // 0xF8 keeps the upper inputs high (they're inputs, value is ignored) and
  // drives the three outputs (0/1/2) LOW — assert resets.
  write8(REG_OUTPUT, 0xF8);
  delay(20);
  // 0xFF releases all three resets — chips boot from here.
  write8(REG_OUTPUT, 0xFF);
  // Touch needs a bit of settle before it ACKs; the retry loop in touchInit
  // handles that on top.
  delay(50);
}

void expanderTouchReset() {
  if (!ok) return;
  // Pin 2 = FT3168 TOUCH_RST (active low). 0xFB = 1111_1011 → only pin 2 LOW
  // (assert touch reset); pin 0 (LCD_RST) and pin 1 (DSI_PWR_EN) stay HIGH so
  // the display is untouched. (Bits 3..7 are inputs, value ignored.)
  write8(REG_OUTPUT, 0xFB);
  delay(20);
  write8(REG_OUTPUT, 0xFF);   // release — the FT3168 reboots from here
  delay(50);
}
