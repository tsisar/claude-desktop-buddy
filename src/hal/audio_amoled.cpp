#include "audio.h"
#include "../board_pins.h"
#include <Arduino.h>
#include <Wire.h>
#include <ESP_I2S.h>

extern "C" {
#include "../audio/es8311.h"
}

// ES8311 + I2S audio for the AMOLED board. Mirrors Waveshare's 15_ES8311
// example init, exposed as a tiny beep API. See audio.h for the caveat:
// this is scaffold — compiled but NOT yet heard on hardware.

#define AUDIO_RATE   16000           // sample rate (Hz)
#define I2C_PORT     0               // Wire's default port (matches Wire.begin)

static I2SClass       i2s;
static es8311_handle_t es = nullptr;
static bool           ok = false;

static bool codecInit() {
  es = es8311_create(I2C_PORT, ES8311_ADDRESS_0);   // CE low -> 0x18
  if (!es) return false;
  const es8311_clock_config_t clk = {
    .mclk_inverted    = false,
    .sclk_inverted    = false,
    .mclk_from_mclk_pin = true,
    .mclk_frequency   = AUDIO_RATE * 256,
    .sample_frequency = AUDIO_RATE,
  };
  if (es8311_init(es, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) return false;
  es8311_sample_frequency_config(es, clk.mclk_frequency, clk.sample_frequency);
  es8311_microphone_config(es, false);
  es8311_voice_volume_set(es, 80, nullptr);
  return true;
}

bool audioInit(TwoWire& /*w*/) {
  // Power-amp enable for the speaker.
  pinMode(AUDIO_PA_EN, OUTPUT);
  digitalWrite(AUDIO_PA_EN, HIGH);

  i2s.setPins(I2S_BCK_IO, I2S_WS_IO, I2S_DO_IO, I2S_DI_IO, I2S_MCK_IO);
  if (!i2s.begin(I2S_MODE_STD, AUDIO_RATE, I2S_DATA_BIT_WIDTH_16BIT,
                 I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    Serial.println("[audio] I2S begin failed");
    return false;
  }
  // Wire is assumed already begun by the caller (shared bus).
  ok = codecInit();
  Serial.printf("[audio] %s\n", ok ? "ES8311 ok" : "ES8311 init FAILED");
  return ok;
}

bool audioOk() { return ok; }

void audioSetVolume(uint8_t pct) {
  if (ok && es) es8311_voice_volume_set(es, pct > 100 ? 100 : pct, nullptr);
}

void audioBeep(uint16_t freq, uint16_t ms) {
  if (!ok || freq == 0) return;
  const int rate = AUDIO_RATE;
  int samples = (int)((uint32_t)rate * ms / 1000);
  if (samples <= 0) return;
  const int period = (freq > 0) ? rate / freq : rate;   // samples per cycle
  const int16_t amp = 6000;                              // ~18% full scale

  // Stream the square wave in small stereo chunks so we don't allocate the
  // whole tone at once.
  static int16_t buf[256];   // 128 stereo frames per chunk
  int done = 0;
  while (done < samples) {
    int frames = 0;
    for (; frames < 128 && done < samples; frames++, done++) {
      int16_t v = ((done / (period / 2 ? period / 2 : 1)) & 1) ? amp : -amp;
      buf[frames * 2]     = v;   // L
      buf[frames * 2 + 1] = v;   // R
    }
    i2s.write((const uint8_t*)buf, frames * 2 * sizeof(int16_t));
  }
}
