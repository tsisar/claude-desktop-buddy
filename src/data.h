#pragma once
#include <stdint.h>

// Bridge data pump: drains USB-CDC + BLE line traffic, parses heartbeat /
// command JSON, and fills TamaState for the UI. Three modes, checked in
// priority order:
//   demo   → auto-cycle fake scenarios every 8s, ignore live data
//   live   → JSON arrived in the last 30s over USB or BLE
//   asleep → no data, all zeros, "No Claude connected"
//
// Declarations only — state and bodies live in data.cpp (this used to be
// header-only with file-static state and the same one-TU-only hazard the
// stats module had).

// Transcript backlog the device retains. The home HUD only shows the last
// few; the full-screen log view scrolls the whole buffer, so keep enough for
// a useful history without blowing the static footprint (N * 92 bytes).
static const uint8_t TAMA_MAX_LINES = 16;

struct TamaState {
  uint8_t  sessionsTotal;
  uint8_t  sessionsRunning;
  uint8_t  sessionsWaiting;
  bool     recentlyCompleted;
  uint32_t tokensToday;
  uint32_t lastUpdated;
  char     msg[24];
  bool     connected;
  char     lines[TAMA_MAX_LINES][92];
  uint8_t  nLines;
  char     promptId[40];     // pending permission request ID; empty = no prompt
  char     promptTool[20];
  char     promptHint[44];
};

void dataSetDemo(bool on);
bool dataDemo();

// True while JSON arrived within the 30s liveness window.
bool dataConnected();

// BLE byte seen within the last 15s (desktop's idle keepalive is ~10s).
bool dataBtActive();

const char* dataScenarioName();

// Set true once the bridge sends a (sanity-checked) time sync — until then
// the RTC may hold whatever was on the coin cell. Seeded at boot from a
// backup-kept hardware RTC (see main setup).
bool dataRtcValid();
void dataSetRtcValid(bool v);

// Drain both channels, dispatch commands, refresh `out`. Call every loop.
void dataPoll(TamaState* out);
