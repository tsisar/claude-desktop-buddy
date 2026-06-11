#include "audio.h"
#include "../board_pins.h"
#include <Arduino.h>
#include <Wire.h>
#include <driver/i2s_std.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <math.h>

extern "C" {
#include "../audio/es8311.h"
}

// ES8311 + I2S audio for the AMOLED board. Reworked to use raw ESP-IDF I2S
// (driver/i2s_std.h) matching Waveshare's 06_I2SCodec example exactly — the
// Arduino ESP_I2S wrapper hard-codes MCLK ×256 and the codec stayed silent;
// the vendor example drives MCLK ×384. See audio.h.
//
// Playback is non-blocking: audioBeep()/audioClick() just enqueue a request
// and a dedicated FreeRTOS task synthesizes the PCM and writes it to I2S, so
// the UI loop never stalls for the duration of a tone. (Pattern borrowed from
// the upstream esp32 build's beepTask.)

#define AUDIO_RATE   16000           // sample rate (Hz)
#define MCLK_MULT    384             // MCLK = rate*384 = 6.144MHz (matches vendor)
#define I2C_PORT     0               // Wire's default port (matches Wire.begin)

// Volume curve. The ES8311 DAC volume reg (0x32) is linear-IN-dB (~0.5 dB/step,
// reg 191 ≈ 0 dB, reg 0 = mute) and the vendored es8311_voice_volume_set maps
// 0..100 straight onto 0..255 — so the bottom half of the scale is −30 dB and
// below, inaudible on this tiny speaker (the "below 60 = silent" surprise;
// upstream just sidesteps it with a fixed AUDIO_VOL=60). We instead compress our
// user-facing 0..100 onto the driver's *audible* span [VOL_MIN_PCT..100] so the
// whole slider does something. Tune on-device: lower VOL_MIN_PCT = quieter floor.
#define VOL_MIN_PCT     60           // our 1% maps to this driver % (the audible floor)
#define DEFAULT_VOLUME  20           // boot level on our 0..100 scale

static i2s_chan_handle_t tx_chan = nullptr;
static es8311_handle_t   es = nullptr;
static bool              ok = false;

// ── async playback queue ──────────────────────────────────────────────────
enum AudioKind : uint8_t { AK_BEEP = 0, AK_CLICK = 1, AK_CLICK_SOFT = 2 };
struct AudioReq { uint8_t kind; uint16_t freq; uint16_t ms; };
static QueueHandle_t s_q = nullptr;

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
  // Output volume is set once in audioInit() via audioSetVolume(DEFAULT_VOLUME)
  // after init succeeds — keep it the single owner; don't set a level here too.
  return true;
}

// ── synthesis (runs on the audio task) ─────────────────────────────────────

// Smooth sine tone — gentler on the amp than a square wave (less DC/harmonics).
static void synthBeep(uint16_t freq, uint16_t ms) {
  if (freq == 0) return;
  const int rate = AUDIO_RATE;
  int samples = (int)((uint32_t)rate * ms / 1000);
  if (samples <= 0) return;
  const float amp    = 6000.0f;                      // ~18% full scale
  const float dphase = 2.0f * (float)M_PI * freq / (float)rate;
  float phase = 0.0f;

  static int16_t buf[256];   // 128 stereo frames per chunk
  int done = 0;
  while (done < samples) {
    int frames = 0;
    for (; frames < 128 && done < samples; frames++, done++) {
      int16_t v = (int16_t)(amp * sinf(phase));
      phase += dphase;
      if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
      buf[frames * 2]     = v;   // L
      buf[frames * 2 + 1] = v;   // R
    }
    i2sWrite(buf, frames * 2 * sizeof(int16_t));
  }
}

// Short percussive "click" (key-press feel): fast exp decay + noisy attack,
// body pitch = freq so approve/deny/menu stay distinct.
static void synthClick(uint16_t freq, bool soft) {
  if (freq == 0) freq = 2000;
  const int rate = AUDIO_RATE;
  const int ms = soft ? 32 : 25;                       // soft = a touch longer
  const int samples = rate * ms / 1000;
  if (samples <= 0) return;
  int period = rate / freq; if (period < 2) period = 2;
  // Sharp click: a noisy attack burst + hard onset gives the percussive bite.
  // Soft click: no noise, the onset is ramped over a few ms and the decay is
  // gentler, so swipes get a rounded tick instead of a snap.
  const int   noiseSamples  = soft ? 0 : rate * 3 / 1000;   // ~3ms attack noise
  const int   attackSamples = soft ? rate * 3 / 1000 : 0;   // ~3ms onset fade-in
  const float noiseAmt      = soft ? 0.0f : 0.6f;
  const float decay         = soft ? 2.6f : 3.5f;
  const float amp           = soft ? 11000.0f : 16000.0f;
  uint32_t rng = 0x9E3779B9u ^ ((uint32_t)freq * 2654435761u);

  static int16_t buf[256];
  int done = 0;
  while (done < samples) {
    int frames = 0;
    for (; frames < 128 && done < samples; frames++, done++) {
      float t   = (float)done / samples;               // 0..1
      float env = expf(-decay * t);                     // percussive decay
      if (attackSamples && done < attackSamples)
        env *= (float)done / attackSamples;             // round off the onset
      // Soft tick uses a pure sine (no harmonics → no "beep" edge); the
      // sharp click keeps the square wave, whose bite suits a snap.
      float tone = soft ? sinf(6.2831853f * freq * (float)done / rate)
                        : (((done % period) < period / 2) ? 1.0f : -1.0f);
      float n = 0.0f;
      if (done < noiseSamples) {                        // click attack
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        n = ((int)(rng & 0xFFFF) - 32768) / 32768.0f;
      }
      float s = (tone * 0.7f + n * noiseAmt) * env;
      int16_t v = (int16_t)(s * amp);
      buf[frames * 2]     = v;
      buf[frames * 2 + 1] = v;
    }
    i2sWrite(buf, frames * 2 * sizeof(int16_t));
  }
}

// The PA + I2S clock tree used to run 24/7 to play ~30 ms clicks: the amp
// sat enabled with its quiescent draw and MCLK/BCLK/WS toggled continuously
// (6.144 MHz MCLK) — a constant multi-mA waste on a 350 mAh cell, plus idle
// hiss. Now the chain powers up on the first queued sound and back down
// after 250 ms of quiet, merging bursts so menu navigation doesn't thrash it.
static void chainUp() {
  digitalWrite(AUDIO_PA_EN, HIGH);
  if (tx_chan) i2s_channel_enable(tx_chan);
  // ~16 ms of silence absorbs the amp's turn-on settle so the first real
  // samples don't ride on a pop.
  static const int16_t zeros[128] = {0};   // 64 stereo frames = 4 ms
  for (int i = 0; i < 4; i++) i2sWrite(zeros, sizeof(zeros));
}

static void chainDown() {
  if (tx_chan) i2s_channel_disable(tx_chan);
  digitalWrite(AUDIO_PA_EN, LOW);
}

static void audioTask(void*) {
  AudioReq r;
  bool up = false;
  for (;;) {
    if (xQueueReceive(s_q, &r, up ? pdMS_TO_TICKS(250) : portMAX_DELAY) == pdTRUE) {
      if (!up) { chainUp(); up = true; }
      if      (r.kind == AK_CLICK)      synthClick(r.freq, false);
      else if (r.kind == AK_CLICK_SOFT) synthClick(r.freq, true);
      else                              synthBeep(r.freq, r.ms);
    } else if (up) {
      chainDown();
      up = false;
    }
  }
}

bool audioInit(TwoWire& /*w*/) {
  // Power-amp enable for the speaker.
  pinMode(AUDIO_PA_EN, OUTPUT);
  digitalWrite(AUDIO_PA_EN, HIGH);

  if (!i2sInit()) return false;
  // Wire is assumed already begun by the caller (shared bus).
  ok = codecInit();
  Serial.printf("[audio] %s\n", ok ? "ES8311 ok" : "ES8311 init FAILED");
  if (!ok) return false;

  // Async playback: one synth task drains the request queue so the UI loop
  // never blocks for the length of a tone.
  s_q = xQueueCreate(8, sizeof(AudioReq));
  if (!s_q) { Serial.println("[audio] queue alloc failed"); ok = false; return false; }
  audioSetVolume(DEFAULT_VOLUME);   // apply the remapped boot level
  // Idle the chain until the first sound: codecInit() above only needed the
  // clocks during register setup; the ES8311 keeps its config over I2C and
  // resyncs to the bit clocks when the task re-enables them.
  chainDown();
  xTaskCreatePinnedToCore(audioTask, "audio", 4096, nullptr, 5, nullptr, tskNO_AFFINITY);
  return true;
}

bool audioOk() { return ok; }

void audioSetVolume(uint8_t pct) {
  if (!ok || !es) return;
  if (pct > 100) pct = 100;
  // Remap 1..100 onto the codec's audible window [VOL_MIN_PCT..100]; 0 = mute.
  int drv = (pct == 0) ? 0 : VOL_MIN_PCT + (100 - VOL_MIN_PCT) * (pct - 1) / 99;
  es8311_voice_volume_set(es, drv, nullptr);
}

void audioBeep(uint16_t freq, uint16_t ms) {
  if (!ok || !s_q || freq == 0) return;
  AudioReq r{ AK_BEEP, freq, ms };
  xQueueSend(s_q, &r, 0);          // non-blocking; drop if the queue is full
}

void audioClick(uint16_t freq) {
  if (!ok || !s_q) return;
  AudioReq r{ AK_CLICK, freq, 0 };
  xQueueSend(s_q, &r, 0);          // non-blocking; drop if the queue is full
}

void audioClickSoft(uint16_t freq) {
  if (!ok || !s_q) return;
  AudioReq r{ AK_CLICK_SOFT, freq, 0 };
  xQueueSend(s_q, &r, 0);          // non-blocking; drop if the queue is full
}