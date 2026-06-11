#pragma once
#include <stdint.h>
class TwoWire;

// FT3168 touch HAL for the Waveshare ESP32-S3-Touch-AMOLED-1.8.
//
// The board has no physical A/B buttons — every UI input goes through this
// chip (addr 0x38). All we need is "is there a finger, and where" — the
// gesture layer on top (gesture.h) turns raw points into tap / swipe.
//
// FT3168 latches a one-shot interrupt on TP_INT, but the panel also keeps
// the touch register valid as long as the finger is down, so polling from
// the main loop works without wiring the IRQ. Stage 3f already proved this.

// Bring up the touch controller on the already-initialised `w` (shared
// I2C bus). Returns false if the chip is missing. Safe to re-call.
bool touchInit(TwoWire& w);
bool touchOk();

// Read current touch. Returns true if a finger is down and fills x/y with
// panel-pixel coordinates (0..LCD_WIDTH-1, 0..LCD_HEIGHT-1). Returns false
// if no finger or an I2C error; outputs untouched in that case.
bool touchRead(uint16_t* x, uint16_t* y);

// Drop the FT3168 into its low-rate monitor scan while the screen is
// blanked (it still reports touches — the double-tap wake keeps working,
// just at the monitor scan rate) and back to full active scan on wake.
// Idempotent; safe to call when touch is absent.
void touchSetLowPower(bool low);