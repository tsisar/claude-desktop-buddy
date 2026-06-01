#include "audio.h"
#include "../board_pins.h"
#include <Arduino.h>
#include <Wire.h>
#include <driver/i2s_std.h>
#include <math.h>

extern "C" {
#include "../audio/es8311.h"
}

// ES8311 + I2S audio for the AMOLED board. Reworked to use raw ESP-IDF I2S
// (driver/i2s_std.h) matching Waveshare's 06_I2SCodec example exactly — the
// Arduino ESP_I2S wrapper hard-codes MCLK ×256 and the codec stayed silent;
// the vendor example drives MCLK ×384. See audio.h.

#define AUDIO_RATE   16000           // sample rate (Hz)
#define MCLK_MULT    384             // MCLK = rate*384 = 6.144MHz (matches vendor)
#define I2C_PORT     0               // Wire's default port (matches Wire.begin)

static i2s_chan_handle_t tx_chan = nullptr;
static es8311_handle_t   es = nullptr;
static bool              ok = false;

static size_t i2sWrite(const void* data, size_t bytes) {
  size_t wrote = 0;
  if (tx_chan) i2s_channel_write(tx_chan, data, bytes, &wrote, 1000);
  return wrote;
}

static bool i2sInit() {
  i2s_chan_config_t chan_cfg = {};
  chan_cfg.id            = I2S_NUM_0;
  chan_cfg.role          = I2S_ROLE_MASTER;
  chan_cfg.dma_desc_num  = 6;
  chan_cfg.dma_frame_num = 240;
  chan_cfg.auto_clear    = true;     // zero the DMA buffer on underrun
  if (i2s_new_channel(&chan_cfg, &tx_chan, nullptr) != ESP_OK) {
    Serial.println("[audio] i2s_new_channel failed");
    return false;
  }

  i2s_std_config_t std_cfg = {};
  std_cfg.clk_cfg.sample_rate_hz = AUDIO_RATE;
  std_cfg.clk_cfg.clk_src        = I2S_CLK_SRC_DEFAULT;
  std_cfg.clk_cfg.mclk_multiple  = (i2s_mclk_multiple_t)MCLK_MULT;

  std_cfg.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT;
  std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO;
  std_cfg.slot_cfg.slot_mode      = I2S_SLOT_MODE_STEREO;
  std_cfg.slot_cfg.slot_mask      = I2S_STD_SLOT_BOTH;
  std_cfg.slot_cfg.ws_width       = I2S_DATA_BIT_WIDTH_16BIT;
  std_cfg.slot_cfg.ws_pol         = false;
  std_cfg.slot_cfg.bit_shift      = true;    // Philips standard
  std_cfg.slot_cfg.left_align     = false;
  std_cfg.slot_cfg.big_endian     = false;
  std_cfg.slot_cfg.bit_order_lsb  = false;

  std_cfg.gpio_cfg.mclk = (gpio_num_t)I2S_MCK_IO;
  std_cfg.gpio_cfg.bclk = (gpio_num_t)I2S_BCK_IO;
  std_cfg.gpio_cfg.ws   = (gpio_num_t)I2S_WS_IO;
  std_cfg.gpio_cfg.dout = (gpio_num_t)I2S_DO_IO;
  std_cfg.gpio_cfg.din  = (gpio_num_t)I2S_DI_IO;
  std_cfg.gpio_cfg.invert_flags.mclk_inv = false;
  std_cfg.gpio_cfg.invert_flags.bclk_inv = false;
  std_cfg.gpio_cfg.invert_flags.ws_inv   = false;

  if (i2s_channel_init_std_mode(tx_chan, &std_cfg) != ESP_OK) {
    Serial.println("[audio] i2s_channel_init_std_mode failed");
    return false;
  }
  if (i2s_channel_enable(tx_chan) != ESP_OK) {
    Serial.println("[audio] i2s_channel_enable failed");
    return false;
  }
  return true;
}

static bool codecInit() {
  es = es8311_create(I2C_PORT, ES8311_ADDRESS_0);   // CE low -> 0x18
  if (!es) return false;
  const es8311_clock_config_t clk = {
    .mclk_inverted    = false,
    .sclk_inverted    = false,
    .mclk_from_mclk_pin = true,
    .mclk_frequency   = AUDIO_RATE * MCLK_MULT,
    .sample_frequency = AUDIO_RATE,
  };
  if (es8311_init(es, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) return false;
  es8311_sample_frequency_config(es, clk.mclk_frequency, clk.sample_frequency);
  es8311_microphone_config(es, false);
  es8311_voice_volume_set(es, 100, nullptr);   // max — debugging audibility
  es8311_register_dump(es);                     // DEBUG: dump codec regs to serial
  return true;
}

bool audioInit(TwoWire& /*w*/) {
  // Power-amp enable for the speaker.
  pinMode(AUDIO_PA_EN, OUTPUT);
  digitalWrite(AUDIO_PA_EN, HIGH);

  if (!i2sInit()) return false;
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

  static int16_t buf[256];   // 128 stereo frames per chunk
  int done = 0;
  size_t wrote = 0;
  while (done < samples) {
    int frames = 0;
    for (; frames < 128 && done < samples; frames++, done++) {
      int16_t v = ((done / (period / 2 ? period / 2 : 1)) & 1) ? amp : -amp;
      buf[frames * 2]     = v;   // L
      buf[frames * 2 + 1] = v;   // R
    }
    wrote += i2sWrite(buf, frames * 2 * sizeof(int16_t));
  }
  static bool logged = false;
  if (!logged) { Serial.printf("[audio] beep wrote %u bytes (PA_EN=%d)\n",
                               (unsigned)wrote, digitalRead(AUDIO_PA_EN)); logged = true; }
}

void audioClick(uint16_t freq) {
  if (!ok) return;
  if (freq == 0) freq = 2000;
  const int rate = AUDIO_RATE;
  const int ms = 25;                                   // tick (boosted for test)
  const int samples = rate * ms / 1000;
  if (samples <= 0) return;
  int period = rate / freq; if (period < 2) period = 2;
  const int noiseSamples = rate * 3 / 1000;            // ~3ms attack noise
  uint32_t rng = 0x9E3779B9u ^ ((uint32_t)freq * 2654435761u);

  static int16_t buf[256];
  int done = 0;
  while (done < samples) {
    int frames = 0;
    for (; frames < 128 && done < samples; frames++, done++) {
      float t   = (float)done / samples;               // 0..1
      float env = expf(-3.5f * t);                      // percussive decay
      float tone = ((done % period) < period / 2) ? 1.0f : -1.0f;
      float n = 0.0f;
      if (done < noiseSamples) {                        // click attack
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        n = ((int)(rng & 0xFFFF) - 32768) / 32768.0f;
      }
      float s = (tone * 0.7f + n * 0.6f) * env;
      int16_t v = (int16_t)(s * 16000.0f);             // boosted for test
      buf[frames * 2]     = v;
      buf[frames * 2 + 1] = v;
    }
    i2sWrite(buf, frames * 2 * sizeof(int16_t));
  }
}
