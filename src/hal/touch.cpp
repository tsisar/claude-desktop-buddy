#include "touch.h"
#include "expander.h"
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

// Touch-failure escalation state. A glitching FT3168 can hold SDA low
// mid-transfer (ESD, rail noise), which stalls EVERY device on the shared bus —
// the AXP2101 included, so battery % reads as -1 and the PWRON key stops firing.
// A single glitch clears in one bus-recover; a HARD-wedged FT3168 never comes
// back from SCL-clocking (verified on hardware — it just re-stalls every loop,
// spamming the bus and the log). So we escalate: try to free the bus a few
// times, and if it stays dead for ~3s, disable touch until the next reboot.
// That frees the bus for the AXP, ends the log spam, and keeps the device
// usable via the physical buttons. A reboot re-inits the chip from scratch.
static uint8_t  failStreak    = 0;
static uint32_t lastRecoverMs = 0;

// Clock SCL up to 9 times to let a slave stuck mid-byte finish and release SDA,
// emit a STOP, then re-init the controller (also clears any hung state in the
// ESP32 I2C driver). Silent — the caller owns logging/escalation.
static void i2cClockBusFree() {
  bus->end();
  pinMode(IIC_SCL, OUTPUT_OPEN_DRAIN);
  pinMode(IIC_SDA, INPUT_PULLUP);
  for (int i = 0; i < 9 && digitalRead(IIC_SDA) == LOW; i++) {
    digitalWrite(IIC_SCL, LOW);  delayMicroseconds(5);
    digitalWrite(IIC_SCL, HIGH); delayMicroseconds(5);
  }
  // STOP condition: SDA low→high while SCL is held high.
  pinMode(IIC_SDA, OUTPUT_OPEN_DRAIN);
  digitalWrite(IIC_SDA, LOW);  delayMicroseconds(5);
  digitalWrite(IIC_SCL, HIGH); delayMicroseconds(5);
  digitalWrite(IIC_SDA, HIGH); delayMicroseconds(5);

  bus->begin(IIC_SDA, IIC_SCL, 400000);
  bus->setTimeOut(50);   // bound any future stall; survives the re-begin
}

// One touch transaction failed. Attempt recovery at most ~2x/sec; give up (and
// disable touch) after a few seconds of unbroken failure so we stop thrashing
// the shared bus and flooding the log.
static void touchFail() {
  if (!bus) return;
  uint32_t now = millis();
  if (now - lastRecoverMs < 500) return;
  lastRecoverMs = now;

  if (++failStreak == 1)
    Serial.println("[touch] FT3168 I2C stalled — recovering shared bus");

  i2cClockBusFree();        // free the bus first so the expander write can land
  expanderTouchReset();     // then HARD-reset the FT3168 (pin 2) — the real fix

  if (failStreak >= 6) {   // ~3s of solid failure — it isn't coming back now
    ok = false;            // stop polling: frees the bus for AXP, ends the spam
    Serial.println("[touch] wedged — paused; auto-retry every 5s "
                   "(buttons still work; suspect FT3168 rail/ALDO3)");
  }
}

// While touch is paused (wedged at runtime, or never found at boot), retry a
// clean re-probe every 5s so a chip that recovers comes back on its own — no
// reboot needed. Cheap: a single ACK probe behind a 50ms timeout, and only
// while ok==false, so it never costs the healthy path anything.
static void touchTryReinit() {
  if (!bus) return;
  static uint32_t lastTryMs = 0;
  uint32_t now = millis();
  if (now - lastTryMs < 5000) return;
  lastTryMs = now;

  i2cClockBusFree();                 // unstick the bus before probing
  expanderTouchReset();              // give the chip a clean hardware reboot
  bus->beginTransmission(TOUCH_ADDR);
  if (bus->endTransmission() == 0) {
    ok = true;
    failStreak = 0;
    Serial.println("[touch] FT3168 back online — re-enabled");
  }
}

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

// FT3168 power-mode register (FocalTech G_PMODE, 0xA5): 0 = active scan,
// 1 = monitor (low-rate autonomous scan, returns to active on touch).
// Hibernate (3) is deliberately not used — leaving it needs a hard reset.
void touchSetLowPower(bool low) {
  static bool curLow = false;
  if (!ok || !bus || low == curLow) return;
  bus->beginTransmission(TOUCH_ADDR);
  bus->write(0xA5);
  bus->write(low ? 0x01 : 0x00);
  if (bus->endTransmission() == 0) curLow = low;
}

bool touchRead(uint16_t* x, uint16_t* y) {
  if (!ok) { touchTryReinit(); return false; }
  // Point register at 0x02 (touch-count), then read the 5-byte block.
  bus->beginTransmission(TOUCH_ADDR);
  bus->write(0x02);
  if (bus->endTransmission(false) != 0) { touchFail(); return false; }
  if (bus->requestFrom((int)TOUCH_ADDR, 5) != 5) { touchFail(); return false; }
  // Transaction went through — clear the failure streak (note the recovery if
  // we'd been struggling, so the log shows the glitch was transient).
  if (failStreak) {
    Serial.printf("[touch] bus recovered after %u tries\n", failStreak);
    failStreak = 0;
  }
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
