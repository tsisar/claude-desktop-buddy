#pragma once
#include <stdint.h>

// Persistent stats backed by NVS. Load once at boot; save sparingly
// (NVS sectors have ~100K write cycles). We save on significant events
// only — approval, denial, nap end, low battery — never on a timer.
//
// Declarations only — state and bodies live in stats.cpp. This used to be
// header-only with file-static state ("include from exactly one TU"), but
// buddy.cpp already included it alongside main.cpp, silently creating a
// second private copy of every static. A real .cpp ends that hazard.

struct Stats {
  uint32_t napSeconds;       // cumulative face-down time
  uint16_t approvals;
  uint16_t denials;
  uint16_t velocity[8];      // ring buffer: seconds-to-respond per approval
  uint8_t  velIdx;
  uint8_t  velCount;
  uint8_t  level;
  uint32_t tokens;          // cumulative output tokens, drives level
};

struct Settings {
  bool sound;
  bool hud;
  bool battery;      // show the battery widget on home / clock
  bool screensaver;  // show the full-screen clock face when idle on USB;
                     // off ⇒ behave as if the RTC were never set (no face,
                     // battery still just sleeps)
};

void statsLoad();
void statsSave();
// Level is token-driven; approvals only feed mood/velocity.
void statsOnApproval(uint32_t secondsToRespond);
// Bridge sends cumulative tokens since its start; deltas feed the pet.
void statsOnBridgeTokens(uint32_t bridgeTotal);
bool statsPollLevelUp();
void statsOnDenial();
void statsMarkDirty();
void statsOnNapEnd(uint32_t seconds);
// Median of the velocity ring buffer. 0 if empty.
uint16_t statsMedianVelocity();
// 0..4 tier. Velocity sets the base; heavy denial ratio drags it down.
uint8_t statsMoodTier();
void statsOnWake();
uint8_t statsEnergyTier();
uint8_t statsFedProgress();

void settingsLoad();
void settingsSave();

// Brightness tier (0..4) persistence — its own key because brightLevel
// lives as a free global in main.cpp. Defaults to 4 (max) on fresh NVS.
uint8_t brightnessLoad();
void brightnessSave(uint8_t v);

void petNameLoad();
void petNameSet(const char* name);
const char* petName();
void ownerSet(const char* name);
const char* ownerName();

uint8_t speciesIdxLoad();
void speciesIdxSave(uint8_t idx);

Settings& settings();
const Stats& stats();
