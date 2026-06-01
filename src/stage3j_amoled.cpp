// Stage 3i.2 + 3i.3 — UI shell + menu / settings / reset modal stack.
//
// What's wired:
//   • Home (CAT species + transcript HUD)  ← 3i.2
//   • Approval screen with swipe APPROVE / DENY  ← 3i.2
//   • Main menu (swipe up to open)  ← 3i.3
//   • Settings sub-menu — tap a row toggles / cycles the value
//   • Reset sub-menu — tap an item opens a confirm modal (replaces M5's
//     tap-twice arm/fire pattern)
//   • Confirm modal — two buttons (Cancel / Confirm), tap outside cancels
//
// Modal stack rules:
//   • swipe up from home    → open main menu
//   • tap a menu row        → execute (apply setting, open sub, etc.)
//   • swipe down anywhere   → step back one level (CONFIRM → RESET →
//                              SETTINGS → MAIN → HOME)
//   • tap outside the panel → close current level
//
// Deferred to later sub-stages:
//   • INFO + PET pages + clock face          → 3i.4
//   • Face-down nap, shake → DIZZY, auto-sleep → 3i.5
//   • Species cycling, GIF characters        → Stage 4
//   • LittleFS for "delete char"             → Stage 4
//   • Real brightness control (LDO ramp)     → 3i.5

#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
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

enum { P_SLEEP, P_IDLE, P_BUSY, P_ATTENTION, P_CELEBRATE, P_DIZZY, P_HEART };

// ── UI state machine ───────────────────────────────────────────────────────
enum UiState {
  UI_NORMAL = 0,
  UI_MENU_MAIN,
  UI_MENU_SETTINGS,
  UI_MENU_RESET,
  UI_CONFIRM,
};
static UiState uiState = UI_NORMAL;

enum ConfirmAction {
  CONF_NONE = 0,
  CONF_DELETE_CHAR,
  CONF_FACTORY_RESET,
};
static ConfirmAction confirmAction = CONF_NONE;

// Highlighted row inside the current modal. Reset to 0 on every state
// change so physical-button navigation (BOOT/PWRON) starts at the top.
// Touch still works independently; the two paths don't conflict because
// the tap path executes immediately without touching navIdx.
static int navIdx = 0;
static void enterState(UiState s) { uiState = s; navIdx = 0; }

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
  // settings().sound gate now reconnected — Settings menu can mute. Audio
  // is currently silent on hardware regardless (see .tmp/backlog.md), but
  // wiring it through here means the moment the codec issue is fixed, the
  // toggle does the right thing without code change.
  if (!settings().sound) return;
  audioBeep(freq, ms);
}

static void sendCmd(const char* json) {
  Serial.println(json);
  size_t n = strlen(json);
  bleWrite((const uint8_t*)json, n);
  bleWrite((const uint8_t*)"\n", 1);
}

// ── word-wrap helper (used by both HUD and approval screen) ────────────────
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

// ── HUD + home (3i.2) ──────────────────────────────────────────────────────
static const int HUD_TOP    = H - 130;
static const int HUD_ROWS   = 3;
static const int HUD_ROW_PX = 18;
static const int HUD_WIDTH  = 36;

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
  StateFn fn = SP->states[activeState];
  if (fn) fn(millis() / 200);
  drawHUD();
}

// ── approval screen (3i.2) ─────────────────────────────────────────────────
static void drawApproval() {
  gfx.fillSprite(0x0000);

  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  gfx.setTextDatum(TC_DATUM);
  gfx.setTextSize(3);
  gfx.setTextColor(waited >= 10 ? 0xFA20 : 0xC618, 0x0000);
  char top[24]; snprintf(top, sizeof(top), "approve?  %lus", (unsigned long)waited);
  gfx.drawString(top, W / 2, 30);

  size_t toolLen = strlen(tama.promptTool);
  gfx.setTextColor(0xFFFF, 0x0000);
  gfx.setTextSize(toolLen <= 12 ? 5 : (toolLen <= 18 ? 4 : 3));
  gfx.drawString(tama.promptTool, W / 2, 100);
  gfx.setTextDatum(TL_DATUM);

  gfx.setTextSize(2);
  gfx.setTextColor(0xC618, 0x0000);
  static char hintLines[6][48];
  uint8_t hn = wrapInto(tama.promptHint, hintLines, 6, HUD_WIDTH);
  for (uint8_t i = 0; i < hn; i++) {
    gfx.setCursor(20, 200 + i * 22);
    gfx.print(hintLines[i]);
  }

  gfx.drawFastHLine(0, H - 80, W, 0x4208);
  gfx.setTextSize(3);
  if (responseSent) {
    gfx.setTextDatum(TC_DATUM);
    gfx.setTextColor(0x8410, 0x0000);
    gfx.drawString("sent...", W / 2, H - 60);
    gfx.setTextDatum(TL_DATUM);
  } else {
    gfx.setTextColor(0x07E0, 0x0000);
    gfx.setCursor(20, H - 60); gfx.print("< APPROVE");
    gfx.setTextColor(0xFA20, 0x0000);
    const char* right = "DENY >";
    int rw = strlen(right) * 6 * 3;
    gfx.setCursor(W - rw - 20, H - 60); gfx.print(right);
  }
}

// ── modal helpers (3i.3) ───────────────────────────────────────────────────
//
// Modal panel: centered, 300 px wide, height grows with item count. Title
// bar at top, then a flat list of rows (label on left, optional value on
// right). Visual style: dark panel, light text, accent border per modal.
//
// menuHit() returns:
//   -2 → tap outside the panel  (caller closes modal)
//   -1 → tap on title / margin  (no-op)
//   ≥0 → tap on item index
static const int MENU_W       = 300;
static const int MENU_X       = (W - MENU_W) / 2;
static const int MENU_TITLE_H = 60;
static const int MENU_ITEM_H  = 36;
static const uint16_t PANEL_BG = 0x18C3;     // very dark grey

static int menuHeight(int items) { return MENU_TITLE_H + items * MENU_ITEM_H + 16; }
static int menuTop(int items)    { return (H - menuHeight(items)) / 2; }

static void drawModal(const char* title, int items, uint16_t border) {
  int mh = menuHeight(items);
  int my = menuTop(items);
  gfx.fillRoundRect(MENU_X, my, MENU_W, mh, 14, PANEL_BG);
  gfx.drawRoundRect(MENU_X, my, MENU_W, mh, 14, border);

  gfx.setTextDatum(TC_DATUM);
  gfx.setTextSize(3);
  gfx.setTextColor(0xFFFF, PANEL_BG);
  gfx.drawString(title, W / 2, my + 16);
  gfx.drawFastHLine(MENU_X + 20, my + MENU_TITLE_H - 8, MENU_W - 40, 0x4208);
  gfx.setTextDatum(TL_DATUM);
}

static void drawMenuRow(int idx, int items, const char* label, const char* value, uint16_t labelCol, uint16_t valueCol) {
  int my = menuTop(items);
  int iy = my + MENU_TITLE_H + idx * MENU_ITEM_H;
  bool selected = (idx == navIdx);
  uint16_t rowBg = PANEL_BG;
  if (selected) {
    rowBg = 0x2104;   // slightly lighter than panel
    gfx.fillRoundRect(MENU_X + 8, iy + 2, MENU_W - 16, MENU_ITEM_H - 4, 6, rowBg);
  }
  gfx.setTextSize(2);
  gfx.setTextColor(labelCol, rowBg);
  gfx.setCursor(MENU_X + 20, iy + 10);
  gfx.print(label);
  if (value && *value) {
    gfx.setTextColor(valueCol, rowBg);
    int vw = strlen(value) * 6 * 2;
    gfx.setCursor(MENU_X + MENU_W - vw - 20, iy + 10);
    gfx.print(value);
  }
}

static int menuHit(int tx, int ty, int items) {
  int my = menuTop(items);
  int mh = menuHeight(items);
  if (tx < MENU_X || tx >= MENU_X + MENU_W) return -2;
  if (ty < my || ty >= my + mh)              return -2;
  int rowsTop = my + MENU_TITLE_H;
  if (ty < rowsTop) return -1;
  int idx = (ty - rowsTop) / MENU_ITEM_H;
  return (idx < items) ? idx : -1;
}

// ── main menu (3i.3) ───────────────────────────────────────────────────────
static const char* const MAIN_ITEMS[] = {
  "settings", "turn off", "help", "about", "demo", "close"
};
static const int MAIN_N = sizeof(MAIN_ITEMS) / sizeof(MAIN_ITEMS[0]);

static void drawMainMenu() {
  drawModal("MENU", MAIN_N, 0x07FF);
  for (int i = 0; i < MAIN_N; i++) {
    const char* value = nullptr;
    if (i == 4) value = dataDemo() ? "on" : "off";
    drawMenuRow(i, MAIN_N, MAIN_ITEMS[i], value, 0xFFFF, 0x07E0);
  }
}

static void applyMainMenu(int idx) {
  switch (idx) {
    case 0:   // settings
      enterState(UI_MENU_SETTINGS);
      break;
    case 1:   // turn off — display sleep; PWRON short tap wakes it back up.
      // Full power-off via AXP2101 OFF register is owned by 3i.5
      // (auto-sleep / wake flow), to keep this stage focused on UI.
      powerSetDisplay(false);
      enterState(UI_NORMAL);
      break;
    case 2:   // help
    case 3:   // about
      // INFO pages land in 3i.4 — stub: just close the menu for now.
      enterState(UI_NORMAL);
      break;
    case 4:   // demo
      dataSetDemo(!dataDemo());
      break;
    case 5:   // close
      enterState(UI_NORMAL);
      break;
  }
}

// ── settings menu (3i.3) ───────────────────────────────────────────────────
static const char* const SETTINGS_ITEMS[] = {
  "brightness", "sound", "bluetooth", "wifi", "led",
  "transcript", "clock rot", "ascii pet", "reset", "back",
};
static const int SETTINGS_N = sizeof(SETTINGS_ITEMS) / sizeof(SETTINGS_ITEMS[0]);
static uint8_t brightLevel = 4;   // 0..4 → applied to panel brightness 20..100%

static void applyBrightness() {
  // Surface::setBrightness() forwards to the SH8601 panel. 0..255 range.
  // Map our 5 tiers to a comfortable curve (dim but readable to full).
  static const uint8_t MAP[5] = { 40, 80, 130, 180, 230 };
  if (dispOk) gfx.setBrightness(MAP[brightLevel]);
}

static void drawSettingsMenu() {
  drawModal("SETTINGS", SETTINGS_N, 0xFFE0);
  Settings& s = settings();
  static const char* const ROT_NAMES[3] = { "auto", "port", "land" };
  for (int i = 0; i < SETTINGS_N; i++) {
    const char* value = nullptr;
    char buf[16];
    bool boolish = false;
    bool boolval = false;
    switch (i) {
      case 0: snprintf(buf, sizeof(buf), "%u/4", brightLevel);   value = buf; break;
      case 1: boolish = true; boolval = s.sound; break;
      case 2: boolish = true; boolval = s.bt;    break;
      case 3: boolish = true; boolval = s.wifi;  break;
      case 4: boolish = true; boolval = s.led;   break;
      case 5: boolish = true; boolval = s.hud;   break;
      case 6: value = ROT_NAMES[s.clockRot < 3 ? s.clockRot : 0]; break;
      case 7:
        // buddySpeciesIdx() / buddySpeciesCount() live in buddy.cpp which
        // isn't linked yet (Stage 4). Leave the value column empty so the
        // row label still reads correctly; cycle becomes a no-op too.
        value = nullptr;
        break;
      // 8/reset and 9/back have no value column
    }
    uint16_t valueCol = 0x07E0;
    if (boolish) {
      value = boolval ? "on" : "off";
      valueCol = boolval ? 0x07E0 : 0x4208;
    }
    drawMenuRow(i, SETTINGS_N, SETTINGS_ITEMS[i], value, 0xFFFF, valueCol);
  }
}

static void applySettings(int idx) {
  Settings& s = settings();
  switch (idx) {
    case 0: brightLevel = (brightLevel + 1) % 5; applyBrightness(); return;
    case 1: s.sound = !s.sound; break;
    case 2: s.bt    = !s.bt;    break;
    case 3: s.wifi  = !s.wifi;  break;
    case 4: s.led   = !s.led;   break;
    case 5: s.hud   = !s.hud;   break;
    case 6: s.clockRot = (s.clockRot + 1) % 3; break;
    case 7:
      // Species cycling stub — only CAT is wired in this stage. Real cycle
      // arrives with the rest of buddies/* loading in Stage 4.
      return;
    case 8:   // reset → open reset sub-menu
      enterState(UI_MENU_RESET);
      return;
    case 9:   // back → main
      enterState(UI_MENU_MAIN);
      return;
  }
  settingsSave();
}

// ── reset menu (3i.3) ──────────────────────────────────────────────────────
static const char* const RESET_ITEMS[] = {
  "delete char", "factory reset", "back",
};
static const int RESET_N = sizeof(RESET_ITEMS) / sizeof(RESET_ITEMS[0]);

static void drawResetMenu() {
  drawModal("RESET", RESET_N, 0xFA20);   // orange — destructive area
  for (int i = 0; i < RESET_N; i++) {
    uint16_t col = (i == RESET_N - 1) ? 0xC618 : 0xFA20;
    drawMenuRow(i, RESET_N, RESET_ITEMS[i], nullptr, col, 0);
  }
}

static void applyReset(int idx) {
  switch (idx) {
    case 0:
      confirmAction = CONF_DELETE_CHAR;
      enterState(UI_CONFIRM);
      return;
    case 1:
      confirmAction = CONF_FACTORY_RESET;
      enterState(UI_CONFIRM);
      return;
    case 2:
      enterState(UI_MENU_SETTINGS);
      return;
  }
}

// ── confirm modal (3i.3) ───────────────────────────────────────────────────
//
// Two-button modal: Cancel (grey) on the left, Confirm (dark red) on the
// right. Tap outside cancels. Border colour matches destructive intent.
static const int CONF_W    = 320;
static const int CONF_H    = 220;
static const int CONF_X    = (W - CONF_W) / 2;
static const int CONF_Y    = (H - CONF_H) / 2;
static const int CONF_BTN_H = 60;
static const int CONF_BTN_PAD = 20;

static const char* confirmTitle() {
  switch (confirmAction) {
    case CONF_DELETE_CHAR:   return "delete char?";
    case CONF_FACTORY_RESET: return "factory reset?";
    default:                 return "are you sure?";
  }
}
static const char* confirmSubtitle() {
  switch (confirmAction) {
    case CONF_DELETE_CHAR:   return "wipes the GIF pack";
    case CONF_FACTORY_RESET: return "wipes NVS + restarts";
    default:                 return "this cannot be undone";
  }
}

static void drawConfirmModal() {
  gfx.fillRoundRect(CONF_X, CONF_Y, CONF_W, CONF_H, 14, PANEL_BG);
  gfx.drawRoundRect(CONF_X, CONF_Y, CONF_W, CONF_H, 14, 0xFA20);

  gfx.setTextDatum(TC_DATUM);
  gfx.setTextSize(3);
  gfx.setTextColor(0xFFFF, PANEL_BG);
  gfx.drawString(confirmTitle(), W / 2, CONF_Y + 22);
  gfx.setTextSize(2);
  gfx.setTextColor(0xC618, PANEL_BG);
  gfx.drawString(confirmSubtitle(), W / 2, CONF_Y + 70);
  gfx.setTextDatum(TL_DATUM);

  int btnY = CONF_Y + CONF_H - CONF_BTN_H - CONF_BTN_PAD;
  int btnW = (CONF_W - 3 * CONF_BTN_PAD) / 2;
  int cancelX  = CONF_X + CONF_BTN_PAD;
  int confirmX = CONF_X + CONF_BTN_PAD * 2 + btnW;

  // Selected button gets a brighter border so BOOT/PWRON navigation is
  // visible without the user having to scrub their finger over the screen.
  uint16_t cancelBorder  = (navIdx == 0) ? 0xFFFF : 0x8410;
  uint16_t confirmBorder = (navIdx == 1) ? 0xFFFF : 0xF800;

  gfx.fillRoundRect(cancelX, btnY, btnW, CONF_BTN_H, 8, 0x4208);
  gfx.drawRoundRect(cancelX, btnY, btnW, CONF_BTN_H, 8, cancelBorder);
  gfx.fillRoundRect(confirmX, btnY, btnW, CONF_BTN_H, 8, 0x7800);
  gfx.drawRoundRect(confirmX, btnY, btnW, CONF_BTN_H, 8, confirmBorder);

  gfx.setTextDatum(MC_DATUM);
  gfx.setTextSize(3);
  gfx.setTextColor(0xFFFF, 0x4208);
  gfx.drawString("Cancel", cancelX + btnW / 2, btnY + CONF_BTN_H / 2);
  gfx.setTextColor(0xFFFF, 0x7800);
  gfx.drawString("Confirm", confirmX + btnW / 2, btnY + CONF_BTN_H / 2);
  gfx.setTextDatum(TL_DATUM);
}

// Returns: -2 outside, -1 dead-zone, 0 cancel, 1 confirm.
static int confirmHit(int tx, int ty) {
  if (tx < CONF_X || tx >= CONF_X + CONF_W) return -2;
  if (ty < CONF_Y || ty >= CONF_Y + CONF_H) return -2;
  int btnY = CONF_Y + CONF_H - CONF_BTN_H - CONF_BTN_PAD;
  if (ty < btnY || ty >= btnY + CONF_BTN_H) return -1;
  int btnW = (CONF_W - 3 * CONF_BTN_PAD) / 2;
  int cancelX  = CONF_X + CONF_BTN_PAD;
  int confirmX = CONF_X + CONF_BTN_PAD * 2 + btnW;
  if (tx >= cancelX  && tx < cancelX  + btnW) return 0;
  if (tx >= confirmX && tx < confirmX + btnW) return 1;
  return -1;
}

static void doConfirm(int decision) {
  if (decision != 1) {
    // cancelled
    enterState(UI_MENU_RESET);
    confirmAction = CONF_NONE;
    return;
  }
  if (confirmAction == CONF_FACTORY_RESET) {
    // NVS namespace wipe + BLE bonds clear. LittleFS format will join when
    // /characters/ comes online in Stage 4.
    Preferences p;
    p.begin("buddy", false);
    p.clear();
    p.end();
    bleClearBonds();
    delay(300);
    ESP.restart();   // does not return
  }
  // CONF_DELETE_CHAR — no-op until LittleFS lands. Acknowledge by closing.
  enterState(UI_NORMAL);
  confirmAction = CONF_NONE;
}

// ── gesture routing ────────────────────────────────────────────────────────
static int currentMenuItems() {
  switch (uiState) {
    case UI_MENU_MAIN:     return MAIN_N;
    case UI_MENU_SETTINGS: return SETTINGS_N;
    case UI_MENU_RESET:    return RESET_N;
    default:               return 0;
  }
}

static void stepBack() {
  switch (uiState) {
    case UI_CONFIRM:       enterState(UI_MENU_RESET);    confirmAction = CONF_NONE; break;
    case UI_MENU_RESET:    enterState(UI_MENU_SETTINGS); break;
    case UI_MENU_SETTINGS: enterState(UI_MENU_MAIN);     break;
    case UI_MENU_MAIN:     enterState(UI_NORMAL);        break;
    default: break;
  }
}

static void handleModalGesture(const GestureEvent& ev) {
  if (ev.kind == GESTURE_SWIPE_DOWN) { stepBack(); return; }

  if (ev.kind != GESTURE_TAP) return;

  if (uiState == UI_CONFIRM) {
    int h = confirmHit(ev.x, ev.y);
    if (h == 0 || h == -2) { doConfirm(0); beep(600, 30); }
    else if (h == 1)       { doConfirm(1); beep(2400, 60); }
    return;
  }

  int idx = menuHit(ev.x, ev.y, currentMenuItems());
  if (idx == -2) {
    enterState(uiState == UI_MENU_MAIN     ? UI_NORMAL
              : uiState == UI_MENU_SETTINGS ? UI_MENU_MAIN
              :                               UI_MENU_SETTINGS);
    beep(600, 30);
    return;
  }
  if (idx < 0) return;

  beep(2400, 30);
  switch (uiState) {
    case UI_MENU_MAIN:     applyMainMenu(idx); break;
    case UI_MENU_SETTINGS: applySettings(idx); break;
    case UI_MENU_RESET:    applyReset(idx);    break;
    default: break;
  }
}

// ── physical buttons (3i.3) ────────────────────────────────────────────────
//
// BOOT (GPIO0, active-low, internal pull-up) is the modal cursor: short tap
// moves the highlight up, long press (≥600 ms) activates the highlighted
// item. PWRON IRQ on the AXP2101 drives the highlight down; a long PWRON
// hold still hits the chip's hardware power-off path, untouched.
//
// Touch stays as a parallel input — small finger-friendly targets are
// hard on the 368×448 panel, hence this physical fallback.
enum BootEvent : uint8_t { BOOT_NONE = 0, BOOT_SHORT, BOOT_LONG };

static BootEvent pollBoot() {
  static bool     wasDown   = false;
  static uint32_t downAt    = 0;
  static bool     longFired = false;
  bool down = (digitalRead(BOOT_BTN) == LOW);
  uint32_t now = millis();
  if (down && !wasDown) {
    wasDown = true;
    downAt = now;
    longFired = false;
    return BOOT_NONE;
  }
  // Fire LONG on the threshold crossing while the key is still held — that
  // way the user feels "I held it, something happened" without having to
  // release first. SHORT only fires on release if LONG didn't already.
  if (down && wasDown && !longFired && (now - downAt) >= 600) {
    longFired = true;
    return BOOT_LONG;
  }
  if (!down && wasDown) {
    wasDown = false;
    uint32_t held = now - downAt;
    if (longFired) return BOOT_NONE;       // already fired LONG; eat release
    if (held >= 30) return BOOT_SHORT;     // debounce <30ms releases
  }
  return BOOT_NONE;
}

static void navUp() {
  int n = currentMenuItems();
  if (uiState == UI_CONFIRM) { navIdx ^= 1; return; }
  if (n <= 0) return;
  navIdx = (navIdx + n - 1) % n;
}
static void navDown() {
  int n = currentMenuItems();
  if (uiState == UI_CONFIRM) { navIdx ^= 1; return; }
  if (n <= 0) return;
  navIdx = (navIdx + 1) % n;
}
static void navActivate() {
  if (uiState == UI_CONFIRM) {
    if (navIdx == 0) { doConfirm(0); beep(600, 30); }
    else             { doConfirm(1); beep(2400, 60); }
    return;
  }
  int n = currentMenuItems();
  if (n <= 0 || navIdx < 0 || navIdx >= n) return;
  beep(2400, 30);
  switch (uiState) {
    case UI_MENU_MAIN:     applyMainMenu(navIdx); break;
    case UI_MENU_SETTINGS: applySettings(navIdx); break;
    case UI_MENU_RESET:    applyReset(navIdx);    break;
    default: break;
  }
}

// ── permission handling ────────────────────────────────────────────────────
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

// ── setup / loop ───────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200); Serial.setTxTimeoutMs(0); delay(300);
  Serial.println("\n[3i.3] UI shell + menu stack");

  Wire.begin(IIC_SDA, IIC_SCL, 400000);
  powerInit(Wire);
  dispOk = gfx.begin();
  imuInit(Wire);
  rtcInit(Wire);
  audioInit(Wire);
  touchInit(Wire);
  pinMode(BOOT_BTN, INPUT_PULLUP);   // physical menu cursor (3i.3)

  statsLoad();
  settingsLoad();
  petNameLoad();
  applyBrightness();

  uint8_t mac[6] = {0}; esp_read_mac(mac, ESP_MAC_BT);
  char name[16]; snprintf(name, sizeof(name), "Claude-%02X%02X", mac[4], mac[5]);
  bleInit(name);

  Serial.printf("[3i.3] disp=%d touch=%d imu=%d rtc=%d pmu=%d  name=%s\n",
                dispOk, touchOk(), imuOk(), rtcOk(), powerOk(), name);
  Serial.printf("[3i.3] audio ok=%d  sound=%d\n", audioOk(), settings().sound);
}

void loop() {
  static uint32_t nextDraw = 0;
  uint32_t now = millis();

  // ── backend pump ──
  dataPoll(&tama);
  if (statsPollLevelUp()) triggerOneShot(P_CELEBRATE, 3000);

  uint8_t base = derive(tama);
  if ((int32_t)(now - oneShotUntil) >= 0) activeState = base;

  if (strcmp(tama.promptId, lastPromptId) != 0) {
    strncpy(lastPromptId, tama.promptId, sizeof(lastPromptId)-1);
    lastPromptId[sizeof(lastPromptId)-1] = 0;
    responseSent = false;
    if (tama.promptId[0]) {
      promptArrivedMs = now;
      beep(1200, 80);
      Serial.printf("[3i.3] PROMPT %s  tool=%s\n", tama.promptId, tama.promptTool);
    }
  }

  // ── physical buttons (parallel to touch) ──
  BootEvent be = pollBoot();
  PwronEvent pw = powerPollButton();

  if (uiState != UI_NORMAL) {
    if      (be == BOOT_SHORT)  { navUp();   beep(1800, 20); }
    else if (be == BOOT_LONG)   { navActivate(); }
    else if (pw == PWRON_SHORT) { navDown(); beep(1800, 20); }
    // PWRON_LONG falls through — AXP2101 owns it (hardware power-off).
  } else {
    // Normal home: PWRON short = screen toggle. BOOT long = open menu
    // as an alternative to the swipe-up gesture, so the device is fully
    // controllable with the physical buttons alone.
    static bool screenOn = true;
    if (pw == PWRON_SHORT) {
      screenOn = !screenOn;
      powerSetDisplay(screenOn);
    }
    if (be == BOOT_LONG) { enterState(UI_MENU_MAIN); beep(800, 60); }
  }

  // ── touch dispatch ──
  gestureUpdate();
  GestureEvent ev = gestureGet();
  bool inPrompt = tama.promptId[0] && !responseSent;

  if (ev.kind != GESTURE_NONE) {
    Serial.printf("[3i.3] gesture %u at (%u,%u) d(%d,%d) state=%d\n",
                  (unsigned)ev.kind, ev.x, ev.y, ev.dx, ev.dy, uiState);
    if (uiState != UI_NORMAL) {
      handleModalGesture(ev);
    } else if (inPrompt) {
      if      (ev.kind == GESTURE_SWIPE_LEFT)  mockApprove();
      else if (ev.kind == GESTURE_SWIPE_RIGHT) mockDeny();
    } else {
      if      (ev.kind == GESTURE_SWIPE_UP) {
        enterState(UI_MENU_MAIN); beep(800, 60);
      } else if (ev.kind == GESTURE_TAP && ev.y >= HUD_TOP) {
        msgScroll = (msgScroll >= 30) ? 0 : msgScroll + 1;
        beep(1800, 30);
      }
    }
  }

  // ── render ──
  if (!dispOk || now < nextDraw) { delay(8); return; }
  nextDraw = now + 200;

  if (inPrompt || responseSent) drawApproval();
  else                          drawHome();

  switch (uiState) {
    case UI_MENU_MAIN:     drawMainMenu();     break;
    case UI_MENU_SETTINGS: drawSettingsMenu(); break;
    case UI_MENU_RESET:    drawResetMenu();    break;
    case UI_CONFIRM:       drawConfirmModal(); break;
    default: break;
  }

  gfx.pushSprite();
  delay(8);
}
