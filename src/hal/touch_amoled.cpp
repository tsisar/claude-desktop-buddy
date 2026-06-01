#include "touch.h"
#include "../board_pins.h"
#include <Arduino.h>
#include <Wire.h>

// FT3168-backed touch HAL. See touch.h for the why.
//
// The FT3168 register map starts at 0x02 for the first touch point:
//   0x02: nibble = number of touches active
//   0x03: x[11:8]  (high nibble masked, the upper bits are gesture id)
//   0x04: x[7:0]
//   0x05: y[11:8]  (same)
//   0x06: y[7:0]
// We only care about point #1; multi-touch isn't used.

static TwoWire* bus = nullptr;
static bool     ok  = false;

bool touchInit(TwoWire& w) {
  if (ok) return true;
  bus = &w;
  pinMode(TP_INT, INPUT);   // not wired to an ISR — kept as a hint pin for now

  // The FT3168 shares the 3.3V (ALDO3) rail with the display. If touchInit
  // runs right after powerInit() turned ALDO3 on, the chip is still booting
  // and won't ACK an I2C probe for the first ~50-150ms. Retry with delays
  // so init order doesn't matter to callers.
  for (int attempt = 0; attempt < 12; attempt++) {
    bus->beginTransmission(TOUCH_ADDR);
    if (bus->endTransmission() == 0) {
      ok = true;
      Serial.printf("[touch] FT3168 ok (probe in %dms)\n", attempt * 25);
      return true;
    }
    delay(25);
  }
  Serial.printf("[touch] FT3168 not found at 0x%02X after 300ms\n", TOUCH_ADDR);
  return false;
}

bool touchOk() { return ok; }

bool touchRead(uint16_t* x, uint16_t* y) {
  if (!ok) return false;
  // Point register at 0x02 (touch-count), then read the 5-byte block.
  bus->beginTransmission(TOUCH_ADDR);
  bus->write(0x02);
  if (bus->endTransmission(false) != 0) return false;
  if (bus->requestFrom((int)TOUCH_ADDR, 5) != 5) return false;
  uint8_t n  = bus->read();
  uint8_t xh = bus->read();
  uint8_t xl = bus->read();
  uint8_t yh = bus->read();
  uint8_t yl = bus->read();
  if ((n & 0x0F) == 0) return false;     // no fingers down
  *x = ((uint16_t)(xh & 0x0F) << 8) | xl;
  *y = ((uint16_t)(yh & 0x0F) << 8) | yl;
  return true;
}
