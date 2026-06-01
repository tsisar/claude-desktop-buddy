#pragma once
#include <stdint.h>
class TwoWire;

// XCA9554 I2C GPIO expander HAL — the bridge between the ESP32-S3 and the
// RESET pins for the FT3168 touch, the SH8601 display, and one more line
// the official Waveshare example resets at boot. The ESP32-S3 GPIOs are
// fully used for QSPI and I2S; reset lines are wired to the expander.
//
// Verified against the vendor's 04_GFX_FT3168_Image example: addr 0x20,
// pins 0/1/2 are OUTPUT and need a low→high pulse at boot for the rest of
// the board to come up clean. Without this, FT3168 occasionally fails to
// ACK on a cold power cycle (which is exactly what the AXP2101 panel-rail
// toggle in powerInit triggers).
//
// This is a 5-register direct-I2C driver — no extra Adafruit library
// needed. Register map (from the TI TCA9554 / XCA9554 datasheet):
//   0x00 input   (read-only)
//   0x01 output  (write to drive the pin)
//   0x02 polarity inversion
//   0x03 configuration (0 = output, 1 = input — default 0xFF, all input)

bool expanderInit(TwoWire& w);
bool expanderOk();

// Drive pins 0/1/2 low for 20 ms, then high. Repeats the verbatim sequence
// from the vendor example. Called from powerInit() — callers don't usually
// invoke this directly.
void expanderResetPulse();
