#pragma once
#include <stdint.h>
class TwoWire;

// Audio HAL for the Waveshare ESP32-S3-Touch-AMOLED-1.8.
//
// This board has NO buzzer — sound goes through the ES8311 codec + speaker
// over I2S (the codec is configured over the shared I2C bus, output enabled
// via the PA_EN pin). audioBeep() synthesizes a short tone and plays it; it's
// the AMOLED replacement for the M5 build's M5.Beep.tone().
//
// NOT YET VERIFIED ON HARDWARE — scaffold only. Compiles; the actual sound
// (codec init + audible tone) needs an on-device listen test.

// Bring up the ES8311 codec + I2S. `w` must already be Wire.begin()'d on the
// shared bus (SDA=15/SCL=14). Returns false if codec init failed.
bool audioInit(TwoWire& w);
bool audioOk();

// Play a square-ish tone: `freq` Hz for `ms` milliseconds. Blocking (writes
// the PCM to I2S and returns when done). No-op if audioInit() failed.
void audioBeep(uint16_t freq, uint16_t ms);

// Play a short percussive "click" (key-press feel): a fast-decaying tick whose
// body pitch is `freq`, with a noisy attack. Much shorter than audioBeep — the
// UI feedback sound. Blocking; no-op if audioInit() failed.
void audioClick(uint16_t freq);

// 0..100 output volume (passed to the codec). Call after audioInit().
void audioSetVolume(uint8_t pct);
