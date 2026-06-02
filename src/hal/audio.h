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
// Playback is non-blocking: audioBeep()/audioClick() enqueue a request that a
// dedicated FreeRTOS task synthesizes and writes to I2S, so the UI loop never
// stalls for the length of a tone.

// Bring up the ES8311 codec + I2S. `w` must already be Wire.begin()'d on the
// shared bus (SDA=15/SCL=14). Returns false if codec init failed.
bool audioInit(TwoWire& w);
bool audioOk();

// Play a sine tone: `freq` Hz for `ms` milliseconds. Non-blocking — queues the
// request to the audio task and returns immediately. No-op if audioInit() failed.
void audioBeep(uint16_t freq, uint16_t ms);

// Play a short percussive "click" (key-press feel): a fast-decaying tick whose
// body pitch is `freq`, with a noisy attack. Much shorter than audioBeep — the
// UI feedback sound. Non-blocking; no-op if audioInit() failed.
void audioClick(uint16_t freq);

// 0..100 output volume (passed to the codec). Call after audioInit().
void audioSetVolume(uint8_t pct);
