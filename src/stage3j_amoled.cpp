// Stage 3i.2 — UI shell over the verified backend:
//   • Home screen: ASCII buddy (CAT for now) up top, transcript HUD below.
//   • Full approval screen on pending prompt: tool / hint / timer / swipe
//     hints / "sent…" state.
//   • Gestures (from src/gesture.h):
//       - swipe LEFT  → APPROVE  (stats hit, beep 2400Hz, heart if <5s)
//       - swipe RIGHT → DENY     (stats hit, beep 600Hz)
//       - tap on HUD  → scroll transcript back one line  (≤30, wraps to 0)
//
// What's deliberately NOT here yet (deferred to later 3i.* sub-stages):
//   • Menu / settings / reset modal     → 3i.3
//   • INFO + PET pages + clock face     → 3i.4
//   • Species cycling, GIF characters   → Stage 4
//   • Auto screen-off / face-down nap / shake → fold into 3i.5
//
// Geometry: 368×448 — full rescale from the M5 135×240 layout.

#include <Arduino.h>
#include <Wire.h>
#include <esp_mac.h>
#include <string.h>
#include "board_pins.h"
#include "hal/display.h"
#include "hal/power.h"
#include "hal/audio.h"
#include "hal/imu.h"
#include "hal/rtc.h"
#include "hal/touch.h"
#include "ble_bridge.h"
#include "data_amoled.h"   // pulls in stats.h
#include "buddy.h"
#include "buddy_common.h"
#include "gesture.h"

static Surface gfx;
static bool dispOk = false;
static const int W = LCD_WIDTH, H = LCD_HEIGHT;

// ── buddy_common.h symbol definitions (Surface backing) ────────────────────
// Same pattern as stage3f; provides the globals the buddies/*.cpp files
// reference. Picked sizes to match the AMOLED panel cleanly.
const int BUDDY_X_CENTER  = W / 2;
const int BUDDY_CANVAS_W  = W;
const int BUDDY_Y_BASE    = 30;
const int BUDDY_Y_OVERLAY = 6;
const int BUDDY_CHAR_W    = 6;
const int BUDDY_CHAR_H    = 8;
const uint16_t BUDDY_BG=0x0000, BUDDY_HEART=0xF810, BUDDY_DIM=0x8410,
  BUDDY_YEL=0xFFE0, BUDDY_WHITE=0xFFFF, BUDDY_CYAN=0x07FF, BUDDY_GREEN=0x07E0,
  BUDDY_PURPLE=0xA01F, BUDDY_RED=0xF800, BUDDY_BLUE=0x041F;
static const uint8_t SCALE = 4;

void buddyPrintLine(const char* line, int yPx, uint16_t color, int xOff) {
  int w = strlen(line) * BUDDY_CHAR_W * SCALE;
  gfx.setTextColor(color, BUDDY_BG);
  gfx.setCursor(BUDDY_X_CENTER - w/2 + xOff*SCALE, yPx);
  gfx.print(line);
}
void buddyPrintSprite(const char* const* lines, uint8_t n, int yOffset, uint16_t color, int xOff) {
  gfx.setTextSize(SCALE);
  int yBase = BUDDY_Y_BASE * SCALE - (SCALE - 1) * 14;
  for (uint8_t i = 0; i < n; i++)
    buddyPrintLine(lines[i], yBase + (yOffset + i*BUDDY_CHAR_H)*SCALE, color, xOff);
}
void buddySetCursor(int x, int y) { gfx.setCursor(BUDDY_X_CENTER + (x-BUDDY_X_CENTER)*SCALE, y*SCALE); }
void buddySetColor(uint16_t fg) { gfx.setTextColor(fg, BUDDY_BG); }
void buddyPrint(const char* s) { gfx.setTextSize(SCALE); gfx.print(s); }

extern const Species CAT_SPECIES;
static const Species* SP = &CAT_SPECIES;

// PersonaState order matches src/main.cpp
enum { P_SLEEP, P_IDLE, P_BUSY, P_ATTENTION, P_CELEBRATE, P_DIZZY, P_HEART };

// ── live state ─────────────────────────────────────────────────────────────
static TamaState tama = {};
static uint8_t   activeState   = P_SLEEP;
static uint32_t  oneShotUntil  = 0;
static uint32_t  promptArrivedMs = 0;
static char      lastPromptId[40] = "";
static bool      responseSent  = false;
static uint8_t   msgScroll     = 0;
static uint16_t  lastLineGen   = 0;

static uint8_t derive(const TamaState& s) {
  if (!s.connected)           return P_IDLE;
  if (s.sessionsWaiting > 0)  return P_ATTENTION;
  if (s.recentlyCompleted)    return P_CELEBRATE;
  if (s.sessionsRunning >= 3) return P_BUSY;
  return P_IDLE;
}

static void triggerOneShot(uint8_t s, uint32_t durMs) {
  activeState = s;
  oneShotUntil = millis() + durMs;
}

static void beep(uint16_t freq, uint16_t ms) {
  // settings().sound gate intentionally removed for now: NVS may hold a
  // stale `false` from earlier experiments, and there's no UI yet to flip
  // it. The mute toggle comes back in stage 3i.3 (settings menu).
  audioBeep(freq, ms);
}

static void sendCmd(const char* json) {
  Serial.println(json);
  size_t n = strlen(json);
  bleWrite((const uint8_t*)json, n);
  bleWrite((const uint8_t*)"\n", 1);
}

// ── word-wrap helper (verbatim from main.cpp, 1px font 6×8) ────────────────
static uint8_t wrapInto(const char* in, char out[][48], uint8_t maxRows, uint8_t width) {
  uint8_t row = 0, col = 0;
  const char* p = in;
  while (*p && row < maxRows) {
    while (*p == ' ') p++;
    const char* w = p;
    while (*p && *p != ' ') p++;
    uint8_t wlen = p - w;
    if (wlen == 0) break;
    uint8_t need = (col > 0 ? 1 : 0) + wlen;
    if (col + need > width) {
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' '; col = 1;
    }
    if (col > 1 || (col == 1 && out[row][0] != ' ')) out[row][col++] = ' ';
    while (wlen > width - col) {
      uint8_t take = width - col;
      memcpy(&out[row][col], w, take); col += take; w += take; wlen -= take;
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' '; col = 1;
    }
    memcpy(&out[row][col], w, wlen); col += wlen;
  }
  if (col > 0 && row < maxRows) { out[row][col] = 0; row++; }
  return row;
}

// ── render: home (buddy + HUD) ─────────────────────────────────────────────
//
// HUD lives in the bottom ~130px. Three visible rows at text size 2
// (16px per row). Fresh transcript line is bright; older rows dimmed.
static const int HUD_TOP    = H - 130;
static const int HUD_ROWS   = 3;
static const int HUD_ROW_PX = 18;
static const int HUD_WIDTH  = 36;   // chars at text size 2 (12px glyph cell on 368px → 30; size 2 fits ~30, give some slack)

static void drawHUD() {
  gfx.fillRect(0, HUD_TOP, W, H - HUD_TOP, 0x0000);
  gfx.drawFastHLine(0, HUD_TOP, W, 0x4208);
  gfx.setTextSize(2);

  if (tama.lineGen != lastLineGen) {
    msgScroll = 0;
    lastLineGen = tama.lineGen;
  }

  if (tama.nLines == 0) {
    gfx.setTextColor(0xC618, 0x0000);
    gfx.setCursor(10, HUD_TOP + 20);
    gfx.print(tama.msg);
    return;
  }

  // Wrap all transcript lines into a flat display buffer (96 max rows is
  // way more than fits on screen — bounds caller's scroll lookup).
  static char disp[96][48];
  static uint8_t srcOf[96];
  uint8_t nDisp = 0;
  for (uint8_t i = 0; i < tama.nLines && nDisp < 96; i++) {
    uint8_t got = wrapInto(tama.lines[i], &disp[nDisp], 96 - nDisp, HUD_WIDTH);
    for (uint8_t j = 0; j < got; j++) srcOf[nDisp + j] = i;
    nDisp += got;
  }
  uint8_t maxBack = (nDisp > HUD_ROWS) ? (nDisp - HUD_ROWS) : 0;
  if (msgScroll > maxBack) msgScroll = maxBack;

  int end = (int)nDisp - msgScroll;
  int start = end - HUD_ROWS; if (start < 0) start = 0;
  uint8_t newest = tama.nLines - 1;
  for (int i = 0; start + i < end; i++) {
    uint8_t row = start + i;
    bool fresh = (srcOf[row] == newest) && (msgScroll == 0);
    gfx.setTextColor(fresh ? 0xFFFF : 0x8C71, 0x0000);
    gfx.setCursor(10, HUD_TOP + 12 + i * HUD_ROW_PX);
    gfx.print(disp[row]);
  }
  if (msgScroll > 0) {
    gfx.setTextColor(0xFFE0, 0x0000);
    gfx.setCursor(W - 60, HUD_TOP + 12);
    gfx.printf("-%u", msgScroll);
  }
}

static void drawHome() {
  gfx.fillSprite(0x0000);
  uint8_t st = activeState;
  StateFn fn = SP->states[st];
  if (fn) fn(millis() / 200);
  drawHUD();
}

// ── render: approval screen ────────────────────────────────────────────────
static void drawApproval() {
  gfx.fillSprite(0x0000);

  // Timer at top: "approve? Ns" — turns red after 10s
  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  gfx.setTextDatum(TC_DATUM);
  gfx.setTextSize(3);
  gfx.setTextColor(waited >= 10 ? 0xFA20 : 0xC618, 0x0000);
  char top[24]; snprintf(top, sizeof(top), "approve?  %lus", (unsigned long)waited);
  gfx.drawString(top, W / 2, 30);

  // Tool name — size depends on length
  size_t toolLen = strlen(tama.promptTool);
  gfx.setTextColor(0xFFFF, 0x0000);
  gfx.setTextSize(toolLen <= 12 ? 5 : (toolLen <= 18 ? 4 : 3));
  gfx.drawString(tama.promptTool, W / 2, 100);
  gfx.setTextDatum(TL_DATUM);

  // Hint wrapped to fixed-width rows in the middle band
  gfx.setTextSize(2);
  gfx.setTextColor(0xC618, 0x0000);
  static char hintLines[6][48];
  uint8_t hn = wrapInto(tama.promptHint, hintLines, 6, HUD_WIDTH);
  for (uint8_t i = 0; i < hn; i++) {
    gfx.setCursor(20, 200 + i * 22);
    gfx.print(hintLines[i]);
  }

  // Footer: swipe hints / sent state
  gfx.drawFastHLine(0, H - 80, W, 0x4208);
  gfx.setTextSize(3);
  if (responseSent) {
    gfx.setTextDatum(TC_DATUM);
    gfx.setTextColor(0x8410, 0x0000);
    gfx.drawString("sent...", W / 2, H - 60);
    gfx.setTextDatum(TL_DATUM);
  } else {
    // Left side: ← APPROVE (green)
    gfx.setTextColor(0x07E0, 0x0000);
    gfx.setCursor(20, H - 60); gfx.print("< APPROVE");
    // Right side: DENY → (red), right-aligned manually
    gfx.setTextColor(0xFA20, 0x0000);
    const char* right = "DENY >";
    int rw = strlen(right) * 6 * 3;
    gfx.setCursor(W - rw - 20, H - 60); gfx.print(right);
  }
}

// ── input handlers ─────────────────────────────────────────────────────────
static void mockApprove() {
  if (!tama.promptId[0]) return;
  char cmd[96];
  snprintf(cmd, sizeof(cmd),
    "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"once\"}", tama.promptId);
  sendCmd(cmd);
  responseSent = true;
  uint32_t tookS = (millis() - promptArrivedMs) / 1000;
  statsOnApproval(tookS);
  beep(2400, 60);
  if (tookS < 5) triggerOneShot(P_HEART, 2000);
}

static void mockDeny() {
  if (!tama.promptId[0]) return;
  char cmd[96];
  snprintf(cmd, sizeof(cmd),
    "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"deny\"}", tama.promptId);
  sendCmd(cmd);
  responseSent = true;
  statsOnDenial();
  beep(600, 60);
}

void setup() {
  Serial.begin(115200); Serial.setTxTimeoutMs(0); delay(300);
  Serial.println("\n[3i.2] UI shell (home + approval)");

  Wire.begin(IIC_SDA, IIC_SCL, 400000);
  powerInit(Wire);     // also pulses XCA9554 → resets touch/display cleanly
  dispOk = gfx.begin();
  imuInit(Wire);
  rtcInit(Wire);
  audioInit(Wire);
  touchInit(Wire);

  statsLoad();
  settingsLoad();
  petNameLoad();

  uint8_t mac[6] = {0}; esp_read_mac(mac, ESP_MAC_BT);
  char name[16]; snprintf(name, sizeof(name), "Claude-%02X%02X", mac[4], mac[5]);
  bleInit(name);

  Serial.printf("[3i.2] disp=%d touch=%d imu=%d rtc=%d pmu=%d  name=%s\n",
                dispOk, touchOk(), imuOk(), rtcOk(), powerOk(), name);
  Serial.printf("[3i.2] audio ok=%d  NVS sound=%d (gate disabled this stage)\n",
                audioOk(), settings().sound);
}

void loop() {
  static uint32_t nextDraw = 0;
  uint32_t now = millis();

  // ── backend pump ──
  dataPoll(&tama);
  if (statsPollLevelUp()) triggerOneShot(P_CELEBRATE, 3000);

  uint8_t base = derive(tama);
  if ((int32_t)(now - oneShotUntil) >= 0) activeState = base;

  // Prompt arrival edge: beep, reset response flag, snapshot timer
  if (strcmp(tama.promptId, lastPromptId) != 0) {
    strncpy(lastPromptId, tama.promptId, sizeof(lastPromptId)-1);
    lastPromptId[sizeof(lastPromptId)-1] = 0;
    responseSent = false;
    if (tama.promptId[0]) {
      promptArrivedMs = now;
      beep(1200, 80);
      Serial.printf("[3i.2] PROMPT %s  tool=%s\n", tama.promptId, tama.promptTool);
    }
  }

  // ── input ──
  gestureUpdate();
  GestureEvent ev = gestureGet();
  bool inPrompt = tama.promptId[0] && !responseSent;

  if (ev.kind != GESTURE_NONE) {
    Serial.printf("[3i.2] gesture %u at (%u,%u) d(%d,%d)\n",
                  (unsigned)ev.kind, ev.x, ev.y, ev.dx, ev.dy);
    if (inPrompt) {
      if      (ev.kind == GESTURE_SWIPE_LEFT)  mockApprove();
      else if (ev.kind == GESTURE_SWIPE_RIGHT) mockDeny();
    } else if (ev.kind == GESTURE_TAP && ev.y >= HUD_TOP) {
      // Cycle scroll-back; 30-line cap matches the M5 build.
      msgScroll = (msgScroll >= 30) ? 0 : msgScroll + 1;
      beep(1800, 30);
    }
  }

  // ── render ──
  if (!dispOk || now < nextDraw) { delay(8); return; }
  nextDraw = now + 200;   // 5fps — buddy frame cadence

  if (inPrompt || responseSent) drawApproval();
  else                          drawHome();

  gfx.pushSprite();
  delay(8);
}
