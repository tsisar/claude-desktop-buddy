// Stage 3i.1 — backend verification: data_amoled.h (heartbeat parser +
// transcript + owner/petname + RTC sync) and stats.h (NVS-backed mood /
// fed / energy / level / tokens). No UI surface yet — the screen shows
// a Serial-style dump of TamaState + Stats so we can watch live data
// flow from a connected Claude desktop without leaving the device.
//
// Verification plan:
//   1) Boot, BLE advertise as "Claude-XXXX". Connect from desktop.
//   2) Heartbeat fills sessionsTotal/Running/Waiting/msg/lines/lineGen.
//   3) Bridge time sync writes RTC; rtcGetTime starts returning real time
//      (replaces the --:--:-- from stage 3g).
//   4) Tokens roll → statsOnBridgeTokens fires; tokens/level on screen
//      increment; level-up triggers a one-time "LEVEL UP" flag.
//   5) Approval prompt arrives → promptId/Tool/Hint shown. PWRON SHORT
//      = mock approve (statsOnApproval + send permission:once). LONG
//      = mock deny (statsOnDenial + send permission:deny). Useful even
//      without touch: we're only proving the backend wiring here, not UI.
//   6) Face-down (QMI8658) → nap-end stats fire when you pick it up.

#include <Arduino.h>
#include <Wire.h>
#include <esp_mac.h>
#include <math.h>
#include "board_pins.h"
#include "hal/display.h"
#include "hal/power.h"
#include "hal/imu.h"
#include "hal/rtc.h"
#include "ble_bridge.h"
#include "data_amoled.h"   // pulls in stats.h transitively

static Surface gfx;
static bool    dispOk = false;
static const int W = LCD_WIDTH, H = LCD_HEIGHT;

static TamaState  tama  = {};
static uint32_t   promptArrivedMs = 0;
static char       lastPromptId[40] = "";
static bool       levelUpFlash    = false;
static uint32_t   levelUpUntil    = 0;

// Face-down nap tracking — same algorithm as stage3g but with stats hook.
static int8_t   faceDownFrames = 0;
static bool     napping        = false;
static uint32_t napStartMs     = 0;

static void sendCmd(const char* json) {
  Serial.println(json);
  size_t n = strlen(json);
  bleWrite((const uint8_t*)json, n);
  bleWrite((const uint8_t*)"\n", 1);
}

static void mockApprove() {
  if (!tama.promptId[0]) return;
  char cmd[96];
  snprintf(cmd, sizeof(cmd),
    "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"once\"}",
    tama.promptId);
  sendCmd(cmd);
  uint32_t tookS = (millis() - promptArrivedMs) / 1000;
  statsOnApproval(tookS);
  Serial.printf("[3i] APPROVE (%lus)\n", (unsigned long)tookS);
  tama.promptId[0] = 0;   // consume locally so PWRON doesn't double-fire
}

static void mockDeny() {
  if (!tama.promptId[0]) return;
  char cmd[96];
  snprintf(cmd, sizeof(cmd),
    "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"deny\"}",
    tama.promptId);
  sendCmd(cmd);
  statsOnDenial();
  Serial.println("[3i] DENY");
  tama.promptId[0] = 0;
}

void setup() {
  Serial.begin(115200); Serial.setTxTimeoutMs(0); delay(300);
  Serial.println("\n[3i] backend verification: data + stats + RTC sync");

  Wire.begin(IIC_SDA, IIC_SCL, 400000);
  powerInit(Wire);       // also resets touch/display via XCA9554
  dispOk = gfx.begin();
  imuInit(Wire);
  rtcInit(Wire);

  statsLoad();
  settingsLoad();
  petNameLoad();

  uint8_t mac[6] = {0}; esp_read_mac(mac, ESP_MAC_BT);
  char name[16]; snprintf(name, sizeof(name), "Claude-%02X%02X", mac[4], mac[5]);
  bleInit(name);

  Serial.printf("[3i] disp=%d imu=%d rtc=%d pmu=%d  name=%s\n",
                dispOk, imuOk(), rtcOk(), powerOk(), name);
  Serial.printf("[3i] loaded: pet=%s owner=%s lvl=%u tokens=%lu\n",
                petName(), ownerName(),
                stats().level, (unsigned long)stats().tokens);
}

void loop() {
  static uint32_t nextDraw = 0;
  uint32_t now = millis();

  // ── data pump ──
  dataPoll(&tama);

  // Prompt arrival edge: record when so stats can credit response time.
  if (strcmp(tama.promptId, lastPromptId) != 0) {
    strncpy(lastPromptId, tama.promptId, sizeof(lastPromptId)-1);
    lastPromptId[sizeof(lastPromptId)-1] = 0;
    if (tama.promptId[0]) {
      promptArrivedMs = millis();
      Serial.printf("[3i] PROMPT %s  tool=%s\n", tama.promptId, tama.promptTool);
    }
  }

  // Level-up flash on screen (3s).
  if (statsPollLevelUp()) { levelUpFlash = true; levelUpUntil = now + 3000; }
  if (now > levelUpUntil) levelUpFlash = false;

  // ── PWRON as mock approve/deny ──
  PwronEvent pw = powerPollButton();
  if      (pw == PWRON_SHORT) mockApprove();
  else if (pw == PWRON_LONG)  mockDeny();

  // ── face-down nap (QMI8658) ──
  static uint32_t nextImu = 0;
  if (now >= nextImu) {
    nextImu = now + 20;
    float ax = 0, ay = 0, az = 0;
    if (imuGetAccel(&ax, &ay, &az)) {
      bool down = (az < -0.7f) && fabsf(ax) < 0.4f && fabsf(ay) < 0.4f;
      if (down) { if (faceDownFrames < 20) faceDownFrames++; }
      else      { if (faceDownFrames > -10) faceDownFrames--; }
      if (!napping && faceDownFrames >= 15) {
        napping = true;
        napStartMs = now;
        Serial.println("[3i] NAP start");
      } else if (napping && faceDownFrames <= -8) {
        napping = false;
        uint32_t napS = (now - napStartMs) / 1000;
        statsOnNapEnd(napS);
        statsOnWake();
        Serial.printf("[3i] NAP end (%lus)\n", (unsigned long)napS);
      }
    }
  }

  // ── render ──
  if (!dispOk || now < nextDraw) { delay(8); return; }
  nextDraw = now + 250;

  gfx.fillSprite(0x0000);
  gfx.setTextDatum(TL_DATUM);

  // BLE + RTC header
  gfx.setTextSize(2);
  gfx.setTextColor(bleConnected() ? 0x07E0 : 0x4208, 0x0000);
  gfx.setCursor(10, 8); gfx.print(bleConnected() ? "BLE linked" : "advertising");

  RtcTime rt; bool rOk = rtcGetTime(&rt);
  gfx.setTextColor(rOk && dataRtcValid() ? 0x07FF : 0x4208, 0x0000);
  gfx.setCursor(W - 130, 8);
  if (rOk) gfx.printf("%02u:%02u:%02u", rt.Hours, rt.Minutes, rt.Seconds);
  else     gfx.print("--:--:--");

  gfx.drawFastHLine(0, 32, W, 0x4208);

  // Live Tama state
  gfx.setTextSize(3);
  gfx.setTextColor(0xFFFF, 0x0000);
  gfx.setCursor(10, 42); gfx.print("CLAUDE");

  gfx.setTextSize(2);
  gfx.setTextColor(0xC618, 0x0000);
  gfx.setCursor(10, 78);
  gfx.printf("sessions  %u  run %u  wait %u",
             tama.sessionsTotal, tama.sessionsRunning, tama.sessionsWaiting);
  gfx.setCursor(10, 100); gfx.printf("via       %s", dataScenarioName());
  gfx.setCursor(10, 122); gfx.printf("tokens/d  %lu", (unsigned long)tama.tokensToday);
  gfx.setCursor(10, 144); gfx.printf("msg       %s", tama.msg);
  gfx.setCursor(10, 166); gfx.printf("lines     %u  gen=%u", tama.nLines, tama.lineGen);

  if (tama.promptId[0]) {
    gfx.setTextSize(3);
    gfx.setTextColor(0xFFE0, 0x0000);
    gfx.setCursor(10, 200); gfx.printf("PROMPT %s", tama.promptTool);
    gfx.setTextSize(2);
    gfx.setTextColor(0xC618, 0x0000);
    gfx.setCursor(10, 235); gfx.printf("%.40s", tama.promptHint);
    gfx.setTextColor(0x4208, 0x0000);
    gfx.setCursor(10, 258); gfx.print("PWRON: short=approve  long=deny");
  } else if (napping) {
    gfx.setTextSize(3);
    gfx.setTextColor(0x041F, 0x0000);
    gfx.setCursor(10, 200); gfx.print("NAPPING");
  } else if (levelUpFlash) {
    gfx.setTextSize(4);
    gfx.setTextColor(0xFFE0, 0x0000);
    gfx.setCursor(10, 200); gfx.print("LEVEL UP!");
  }

  gfx.drawFastHLine(0, 295, W, 0x4208);

  // Stats panel
  gfx.setTextSize(3);
  gfx.setTextColor(0xFFFF, 0x0000);
  gfx.setCursor(10, 305); gfx.print("STATS");

  gfx.setTextSize(2);
  gfx.setTextColor(0xC618, 0x0000);
  gfx.setCursor(10, 342);
  gfx.printf("lvl %u   tokens %lu",
             stats().level, (unsigned long)stats().tokens);
  gfx.setCursor(10, 364);
  gfx.printf("mood %u   energy %u   fed %u/10",
             statsMoodTier(), statsEnergyTier(), statsFedProgress());
  gfx.setCursor(10, 386);
  gfx.printf("appr %u   deny %u",
             stats().approvals, stats().denials);
  uint32_t nap = stats().napSeconds;
  gfx.setCursor(10, 408);
  gfx.printf("napped %luh %02lum", nap / 3600, (nap / 60) % 60);

  gfx.setTextSize(2);
  gfx.setTextColor(0x4208, 0x0000);
  gfx.setCursor(10, H - 22);
  gfx.printf("pet=%s  owner=%s", petName(), ownerName());

  gfx.pushSprite();
  delay(8);
}
