// Stage 3i.2 + 3i.3 — UI shell + menu / settings / reset modal stack.
//
// What's wired:
//   • Home (CAT species + transcript HUD)  ← 3i.2
//   • Approval screen with swipe APPROVE / DENY  ← 3i.2
//   • Main menu (BOOT tap to open; BOOT hold saves a screenshot)  ← 3i.3
//   • Settings sub-menu — tap a row toggles / cycles the value
//   • Reset sub-menu — tap an item opens a confirm modal (replaces M5's
//     tap-twice arm/fire pattern)
//   • Confirm modal — two buttons (Cancel / Confirm), tap outside cancels
//
// Modal stack rules:
//   • BOOT tap              → open main menu / activate highlighted item
//   • BOOT hold (any state) → screenshot to SD (screenshot.h)
//   • PWRON tap in a modal  → wrap the highlight down through the items
//   • swipe up in a modal   → close the menu
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
#include <esp_system.h>   // esp_reset_reason() — hang breadcrumb at boot
#include <string.h>
#include <math.h>
#include "board_pins.h"
#include "hal/display.h"
#include "hal/power.h"
#include "hal/battery.h"
#include "hal/audio.h"
#include "hal/imu.h"
#include "hal/rtc.h"
#include "hal/touch.h"
#include "hal/storage.h"
#include "screenshot.h"
#include "ble_bridge.h"
#include "data.h"
#include "stats.h"
#include "buddy.h"
#include "buddy_common.h"
#include "blockart.h"
#include "gesture.h"
#include "debug.h"

// gifAvailable / buddyMode globals — xfer.h references both via
// extern, and the home render path below picks between buddyTick() and
// characterTick() based on them.
bool buddyMode    = true;    // ASCII species mode is the boot default
bool gifAvailable = false;   // flipped to true when characterInit succeeds

#include "character.h"      // characterInit/Close/Tick/SetState/Loaded
#include "svg_anim.h"       // svgAnimLoaded() — faster render cadence for SVG
#include "clock.h"          // clockDrawWidget/clockDrawFace — clock UI
#include "xfer.h"           // xferCommand() — calls characterInit on char_end

// Non-static so character.cpp can reach it via extern. Everything
// else in this file still treats it as private.
Surface gfx;
static bool dispOk = false;
static const int W = LCD_WIDTH, H = LCD_HEIGHT;
// Extra inset for corner-anchored UI so it clears the panel's rounded glass.
// Bump this if elements still clip in the corners.
static const int SAFE = 4;

// ── buddy_common.h symbol definitions (Surface backing) ────────────────────
const int BUDDY_X_CENTER  = W / 2;
const int BUDDY_CANVAS_W  = W;
// Logical body anchor. Also used by species as the lower clip bound for
// particles (e.g. `y > BUDDY_Y_BASE + 20`), so it stays at its design value
// of 30 — do NOT nudge it to move the buddy down; use BUDDY_Y_SHIFT instead.
const int BUDDY_Y_BASE    = 30;
const int BUDDY_Y_OVERLAY = 6;
const int BUDDY_CHAR_W    = 6;
const int BUDDY_CHAR_H    = 8;
const uint16_t BUDDY_BG=0x0000, BUDDY_HEART=0xF810, BUDDY_DIM=0x8410,
  BUDDY_YEL=0xFFE0, BUDDY_WHITE=0xFFFF, BUDDY_CYAN=0x07FF, BUDDY_GREEN=0x07E0,
  BUDDY_PURPLE=0xA01F, BUDDY_RED=0xF800, BUDDY_BLUE=0x041F;
static const uint8_t SCALE = 4;
// Horizontal text scale, deliberately smaller than the vertical SCALE.
// The species art was authored against terminal proportions (cells ~1:2,
// tall and narrow); the classic 6×8 GFX cell scaled uniformly is 1:1.33
// and reads visibly too WIDE. 3:4 gives an 18×32 cell (1:1.78) — close
// to terminal aspect without touching any vertical layout constants, so
// the per-species yOffset/overlay magic numbers all stay valid.
static const uint8_t SCALE_X = 3;

// ── hang breadcrumb (diagnostic) ───────────────────────────────────────────
// RTC_NOINIT survives a warm reset but is wiped by a real power-cut. The
// loop-WDT (5s, warm panic-reboot) trips BEFORE the AXP2101 hardware WDT (8s,
// hard rail cut), so a wedged loop is caught warm first and the last stage
// loop() entered survives here to be printed on the next boot — naming the
// subsystem that hung. Only a deeper failure (the panic handler itself stuck)
// lets the AXP do its hard cut, which wipes this; that's the truly
// unrecoverable case the AXP backstop exists for.
enum LoopStage : uint8_t {
  LS_DATAPOLL = 1, LS_IMU, LS_AUTOOFF, LS_BLE, LS_BUTTONS,
  LS_TOUCH, LS_RENDER, LS_PUSH,
};
static const char* const LS_NAME[] = {
  "?", "dataPoll", "imu", "autoOff", "ble", "buttons",
  "touch", "render", "pushSprite",
};
RTC_NOINIT_ATTR static uint32_t bcMagic;
RTC_NOINIT_ATTR static uint8_t  bcStage;
RTC_NOINIT_ATTR static uint32_t bcLoops;
static const uint32_t BC_MAGIC = 0xB0FFEE01;
static inline void bc(uint8_t s) { bcStage = s; }
// Human-readable summary of the *previous* boot's hang, filled once at setup()
// from the surviving breadcrumb. Empty on a clean cold boot. Shown on the
// INFO/DEVICE page so a field repro (no serial attached) is still diagnosable.
static char lastHang[40] = "";

// Whole-buddy vertical offset, in pixels, applied uniformly to the sprite
// body AND every particle. This is the knob for "push all buddies down so
// they clear the top battery/clock HUD" — it moves body and overlays as one
// rigid unit, unlike BUDDY_Y_BASE which only shifts the body. The HUD is
// drawn outside these helpers, so it stays put.
static const int BUDDY_Y_SHIFT = 32;

void buddyPrintLine(const char* line, int yPx, uint16_t color, int xOff) {
  int w = strlen(line) * BUDDY_CHAR_W * SCALE_X;
  gfx.setTextColor(color, BUDDY_BG);
  gfx.setCursor(BUDDY_X_CENTER - w/2 + xOff*SCALE_X, yPx + BUDDY_Y_SHIFT);
  gfx.print(line);
}
void buddyPrintSprite(const char* const* lines, uint8_t n, int yOffset, uint16_t color, int xOff) {
  gfx.setTextSize(SCALE_X, SCALE);
  int yBase = BUDDY_Y_BASE * SCALE - (SCALE - 1) * 14;
  for (uint8_t i = 0; i < n; i++)
    buddyPrintLine(lines[i], yBase + (yOffset + i*BUDDY_CHAR_H)*SCALE, color, xOff);
}
void buddyPrintBlocks(const char* const* lines, uint8_t n, int yOffset, uint16_t color, int xOff) {
  int yBase = BUDDY_Y_BASE * SCALE - (SCALE - 1) * 14;
  for (uint8_t i = 0; i < n; i++)
    blockart::drawLineCentered(gfx, lines[i],
                               BUDDY_X_CENTER + xOff * SCALE_X,
                               yBase + (yOffset + i*BUDDY_CHAR_H)*SCALE + BUDDY_Y_SHIFT,
                               SCALE_X, SCALE, color, BUDDY_BG);
}
void buddySetCursor(int x, int y) { gfx.setCursor(BUDDY_X_CENTER + (x-BUDDY_X_CENTER)*SCALE_X, y*SCALE + BUDDY_Y_SHIFT); }
void buddySetColor(uint16_t fg) { gfx.setTextColor(fg, BUDDY_BG); }
void buddyPrint(const char* s) { gfx.setTextSize(SCALE_X, SCALE); gfx.print(s); }

// Species registry + buddyTick live in src/buddy.cpp now (Stage 4).
// Stage3j just drives the state machine and calls buddyTick(activeState).

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

// Display mode is orthogonal to modal stack: it picks WHAT renders
// underneath the modal layer when uiState == UI_NORMAL.
//   NORMAL → buddy + HUD (and clock face when conditions match)
//   PET    → 2 pages: stats / how-to
//   INFO   → 6 pages: ABOUT / BUTTONS / CLAUDE / DEVICE / BLUETOOTH / CREDITS
//   TRANSCRIPT → full-screen scrollable log (tap the HUD to open; not cycled)
// NORMAL/PET/INFO cycle with swipe-up/down or BOOT short. Pagination inside
// PET/INFO via swipe-left/right or PWRON-short/BOOT-short.
enum DispMode {
  DISP_NORMAL = 0,
  DISP_PET,
  DISP_INFO,
  DISP_COUNT,
  DISP_TRANSCRIPT,   // full-screen log; reached by tapping the HUD, not cycled
};
static DispMode displayMode = DISP_NORMAL;
static uint8_t  petPage  = 0;
static uint8_t  infoPage = 0;
static const uint8_t PET_PAGES  = 2;
static const uint8_t INFO_PAGES = 6;

// Brightness lives up here so drawInfoDevice can read it. Apply funcs and
// the settings-menu cycle path are defined further down with the rest of
// the settings logic.
static uint8_t brightLevel = 4;

// PWRON short-press flips this. applyBrightness() honours it — sets the
// SH8601 brightness register to 0 when off, which goes dark instantly
// (rail toggle alone doesn't, the panel holds the last frame on its caps
// for a beat). Rails stay live so touch keeps working and a second
// PWRON-short brings the screen straight back without expander re-init.
static bool screenOn = true;

// Render pacing deadline. File-scope (not a loop() static) so wake() and
// the PWRON toggle can zero it — the first frame after a relight renders
// on the next loop pass instead of showing stale pixels for up to 200 ms.
static uint32_t nextDraw = 0;
// Parked clock face dims to the lowest brightness tier (AOD) — burn-in
// protection for the days-long USB park. Owned by the render path below.
static bool aodActive = false;
// SH8601 is in SLPIN while blanked; applyBrightness()/shotFlash() own the
// transitions (sleep in/out block ~240 ms each, paid once per blank/wake).
static bool panelAsleep = false;

// BLE advertise name "Claude-XXXX" — populated in setup() and surfaced on
// the INFO/BLUETOOTH page so the user knows which device is theirs when
// multiple are in range.
static char btName[16] = "Claude";

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
static uint8_t   tScroll       = 0;   // transcript view: lines scrolled back from the tail
// Approval swipe feedback: a brief edge glow on the side you swiped —
// green/right = approve, red/left = deny. Side: +1 right, -1 left, 0 none.
static uint32_t  decisionFlashUntil = 0;
static int8_t    decisionFlashSide  = 0;

// ── auto-behaviours (3i.5) ─────────────────────────────────────────────────
static uint32_t lastInteractMs    = 0;   // any input or prompt arrival
static uint32_t wakeTransitionMs  = 0;   // until: hold SLEEP after waking
static uint32_t lastShakeCheckMs  = 0;   // throttle IMU read to ~50 Hz
static float    accelBaseline     = 1.0f; // EMA for shake delta
static int8_t   faceDownFrames    = 0;   // debounce: enter +15, exit -8
static bool     napping           = false;
static uint32_t napStartMs        = 0;
static uint32_t lastPasskey       = 0;
static uint32_t lastWakeTapMs     = 0;   // double-tap-to-wake: time of 1st tap
static const uint32_t DOUBLE_TAP_MS = 400;  // max gap between the two taps
// Single idle timeout governs both screen-saver behaviours: after this much
// time with no interaction the home view is "parked" — on USB it becomes the
// clock face, on battery the panel turns off. Any input (touch / button /
// shake / new prompt) resets it via wake().
static const uint32_t IDLE_MS = 30000;
// On battery the screensaver clock face shows for this long after IDLE_MS,
// then the panel turns off to save charge. On USB the face stays up.
static const uint32_t BATTERY_CLOCK_MS = 60000;

static uint8_t derive(const TamaState& s) {
  if (!s.connected)           return P_IDLE;
  if (s.sessionsWaiting > 0)  return P_ATTENTION;
  if (s.recentlyCompleted)    return P_CELEBRATE;
  if (s.sessionsRunning >= 1) return P_BUSY;   // any active session = busy
  return P_IDLE;
}

static void triggerOneShot(uint8_t s, uint32_t durMs) {
  activeState = s;
  oneShotUntil = millis() + durMs;
}

// Forward — applyBrightness() body lives down with the settings code, but
// wake()/IMU/auto-screen-off paths up here need to be able to call it.
static void applyBrightness();

// Mark the device as interacted-with: arms the 30s auto-screen-off timer
// and, if the panel was dark, brings it back. A 12-second SLEEP-hold is
// armed so the wake-up animation gets visible airtime before normal state
// dispatch takes over.
static void wake() {
  lastInteractMs = millis();
  if (!screenOn) {
    screenOn = true;
    applyBrightness();
    wakeTransitionMs = millis() + 12000;
    nextDraw = 0;   // render the first frame on the next pass, not +200 ms
  }
}

// Debug hook for the test bridges' cmd:pose (see xfer.h) — hold any persona
// state on screen for buddy-art iteration, without faking the protocol
// traffic that would naturally produce it (HEART, for one, normally shows
// for only 2s after a fast approve).
void debugPose(uint8_t s, uint32_t durMs) {
  if (s > P_HEART) return;
  triggerOneShot(s, durMs);
  wake();   // relight the panel so the pose is actually visible
}

static void beep(uint16_t freq, uint16_t ms) {
  // TEMP: back to the original square-wave tone to test basic audibility.
  if (!settings().sound) return;
  audioBeep(freq, ms);
}

// Short percussive tick (noisy attack + fast decay) — distinct from a tone
// beep. Used as the screenshot "shutter" sound. Gated by the same setting.
static void click(uint16_t freq) {
  if (!settings().sound) return;
  audioClick(freq);
}

// Softer tick (no noise, ramped onset) for swipe navigation.
static void clickSoft(uint16_t freq) {
  if (!settings().sound) return;
  audioClickSoft(freq);
}

// Send a device-originated command (permission decisions) to the host.
// The prompt can arrive over USB or BLE, so the reply goes out on BOTH
// channels unconditionally — routing the Serial side through the verbose-
// only log macro silently dropped every USB reply in normal builds (same
// bug class xfer.h's _xReply already fixed for acks). The TX-timeout
// bracket mirrors _xReply: boot sets setTxTimeoutMs(0) so stray logs never
// block with no host attached, but a protocol reply must not be dropped
// on a full TX ring.
static void sendCmd(const char* json) {
  size_t n = strlen(json);
  Serial.setTxTimeoutMs(50);
  Serial.write((const uint8_t*)json, n);
  Serial.write((const uint8_t*)"\n", 1);
  Serial.setTxTimeoutMs(0);
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
      // Snap the break back to a UTF-8 char boundary so a multi-byte glyph
      // isn't split (which would render as a garbage box).
      while (take > 0 && ((unsigned char)w[take] & 0xC0) == 0x80) take--;
      if (take == 0) take = width - col;   // single glyph wider than line
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

// Wrap scratch shared by drawHUD and drawTranscript — at most one of them
// renders per frame, and two private static copies wasted 4.7 KB of
// internal SRAM each.
static char    wrapDisp[96][48];
static uint8_t wrapSrc[96];

// Home HUD: a compact, always-live tail of the transcript — the latest
// HUD_ROWS wrapped lines, newest at the bottom in white, older greyed. No
// manual scrolling here; tapping the strip opens the full-screen log
// (DISP_TRANSCRIPT) where history can be browsed.
static void drawHUD() {
  gfx.fillRect(0, HUD_TOP, W, H - HUD_TOP, 0x0000);
  gfx.drawFastHLine(0, HUD_TOP, W, 0x4208);
  gfx.setTextSize(2);

  if (tama.nLines == 0) {
    gfx.setTextColor(0xC618, 0x0000);
    gfx.setCursor(10, HUD_TOP + 20);
    gfx.print(tama.msg);
    return;
  }

  uint8_t nDisp = 0;
  for (uint8_t i = 0; i < tama.nLines && nDisp < 96; i++) {
    uint8_t got = wrapInto(tama.lines[i], &wrapDisp[nDisp], 96 - nDisp, HUD_WIDTH);
    for (uint8_t j = 0; j < got; j++) wrapSrc[nDisp + j] = i;
    nDisp += got;
  }
  int start = (int)nDisp - HUD_ROWS; if (start < 0) start = 0;
  uint8_t newest = tama.nLines - 1;
  for (int i = 0; start + i < (int)nDisp; i++) {
    uint8_t row = start + i;
    bool fresh = (wrapSrc[row] == newest);
    gfx.setTextColor(fresh ? 0xFFFF : 0x8C71, 0x0000);
    gfx.setCursor(10, HUD_TOP + 12 + i * HUD_ROW_PX);
    gfx.print(wrapDisp[row]);
  }
}

// Full-screen transcript log. Newest line at the bottom (chat order); the
// tail is "live" (tScroll == 0). Swipe up = older, swipe down = newer, tap
// closes back to home. Reached by tapping the home HUD.
static const int TRANS_TOP  = 54;
static const int TRANS_ROWS = (H - TRANS_TOP - 28) / HUD_ROW_PX;

static void drawTranscript() {
  gfx.fillSprite(0x0000);
  gfx.setTextSize(3);
  gfx.setTextColor(0xFFFF, 0x0000);
  gfx.setCursor(10 + SAFE, 12 + SAFE); gfx.print("LOG");
  gfx.setTextSize(2);
  gfx.setTextColor(0x07E0, 0x0000);
  gfx.setTextDatum(TR_DATUM);
  gfx.drawString("swipe ^ = close", W - 10 - SAFE, 18 + SAFE);
  gfx.setTextDatum(TL_DATUM);
  gfx.drawFastHLine(0, 44, W, 0x4208);

  gfx.setTextSize(2);
  if (tama.nLines == 0) {
    gfx.setTextColor(0xC618, 0x0000);
    gfx.setCursor(10, TRANS_TOP); gfx.print(tama.msg);
    return;
  }

  uint8_t nDisp = 0;
  for (uint8_t i = 0; i < tama.nLines && nDisp < 96; i++) {
    uint8_t got = wrapInto(tama.lines[i], &wrapDisp[nDisp], 96 - nDisp, HUD_WIDTH);
    for (uint8_t j = 0; j < got; j++) wrapSrc[nDisp + j] = i;
    nDisp += got;
  }
  uint8_t maxBack = (nDisp > TRANS_ROWS) ? (nDisp - TRANS_ROWS) : 0;
  if (tScroll > maxBack) tScroll = maxBack;

  int end = (int)nDisp - tScroll;
  int start = end - TRANS_ROWS; if (start < 0) start = 0;
  uint8_t newest = tama.nLines - 1;
  for (int i = 0; start + i < end; i++) {
    uint8_t row = start + i;
    bool fresh = (wrapSrc[row] == newest) && (tScroll == 0);
    gfx.setTextColor(fresh ? 0xFFFF : 0xC618, 0x0000);
    gfx.setCursor(10, TRANS_TOP + i * HUD_ROW_PX);
    gfx.print(wrapDisp[row]);
  }

  gfx.setTextDatum(BC_DATUM);
  if (tScroll > 0) {
    gfx.setTextColor(0xFFE0, 0x0000);
    char b[28]; snprintf(b, sizeof(b), "-%u   swipe v = older", tScroll);
    gfx.drawString(b, W / 2, H - 6);
  } else {
    gfx.setTextColor(0x07E0, 0x0000);
    gfx.drawString("LIVE   swipe v = older", W / 2, H - 6);
  }
  gfx.setTextDatum(TL_DATUM);
}

void drawBatteryWidget(int bx, int by);          // non-static: clock.cpp overlays it

// Install progress card: shown over the buddy while xfer.h is mid-transfer.
// Replaces the buddy view so the user knows they should leave the desktop
// alone. Kilobyte counts + a bar; updates every render tick (5 fps).
static void drawXferProgress() {
  gfx.fillSprite(0x0000);
  uint32_t done  = xferProgress();
  uint32_t total = xferTotal();

  gfx.setTextDatum(TC_DATUM);
  gfx.setTextSize(4);
  gfx.setTextColor(0xFFE0, 0x0000);
  gfx.drawString("installing", W / 2, H / 2 - 90);

  gfx.setTextSize(3);
  gfx.setTextColor(0xFFFF, 0x0000);
  char b[24];
  if (total > 0) snprintf(b, sizeof(b), "%luK / %luK", done / 1024, total / 1024);
  else           snprintf(b, sizeof(b), "%luK", done / 1024);
  gfx.drawString(b, W / 2, H / 2 - 30);
  gfx.setTextDatum(TL_DATUM);

  // Progress bar
  const int bx = 40, by = H / 2 + 20, bw = W - 80, bh = 30;
  gfx.drawRoundRect(bx, by, bw, bh, 6, 0x8410);
  if (total > 0 && done > 0) {
    int fillW = (int)((uint64_t)(bw - 4) * done / total);
    if (fillW > 1) gfx.fillRoundRect(bx + 2, by + 2, fillW, bh - 4, 4, 0x07E0);
  }

  gfx.setTextSize(2);
  gfx.setTextColor(0xC618, 0x0000);
  gfx.setTextDatum(TC_DATUM);
  gfx.drawString("don't unplug", W / 2, H - 70);
  gfx.setTextDatum(TL_DATUM);
}

static void drawHome() {
  gfx.fillSprite(0x0000);
  if (gifAvailable && !buddyMode && characterLoaded()) {
    // GIF character mode — push the buddy state into the renderer and
    // let it decode the next frame for the current persona.
    characterSetState(activeState);
    characterTick();
  } else {
    buddyTick(activeState);   // ASCII species path
  }
  if (settings().hud) drawHUD();   // "transcript" setting gates the HUD
  drawBatteryWidget(W - 14 - 14 - SAFE, 14 + SAFE);   // top-right, alongside the buddy
  if (settings().battery && dataRtcValid())
    clockDrawWidget(SAFE + 2, 14 + SAFE + 17);        // top-left, mirrors the battery
}

// ── approval screen (3i.2) ─────────────────────────────────────────────────
static void drawApproval() {
  gfx.fillSprite(0x0000);

  // Keep the buddy alive during a prompt: animate it in ATTENTION (the
  // pulsing "!" mood) up top, exactly like the home view, then lay the
  // approval details over a card along the bottom.
  if (gifAvailable && !buddyMode && characterLoaded()) {
    characterSetState(P_ATTENTION);
    characterTick();
  } else {
    buddyTick(P_ATTENTION);
  }
  // Same top HUD as the home view — battery/clock stay visible during a
  // prompt (a top banner there proved unreadable, so the corners are free).
  drawBatteryWidget(W - 14 - 14 - SAFE, 14 + SAFE);   // top-right
  if (settings().battery && dataRtcValid())
    clockDrawWidget(SAFE + 2, 14 + SAFE + 17);        // top-left

  // ── approval card (bottom third) ──
  const int cardTop = 296;
  gfx.fillRect(0, cardTop, W, H - cardTop, 0x0000);   // mask any buddy bleed
  gfx.drawFastHLine(0, cardTop, W, 0x4208);

  // "approve? Ns" countdown — right below the buddy, ABOVE the card line.
  // It used to ride as a banner along the very top, but up there it fought
  // the rounded glass and was hard to read. Mask its band first so a tall
  // buddy frame can't bleed into the text.
  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  gfx.fillRect(0, cardTop - 34, W, 34, 0x0000);
  gfx.setTextSize(3);
  // Faux bold: the band above is freshly masked black, so draw the string
  // twice with a 1 px x-shift, transparent bg (the opaque-bg variant would
  // erase the first pass).
  gfx.setTextColor(waited >= 10 ? 0xFA20 : 0xC618);
  gfx.setTextDatum(TC_DATUM);
  char top[24]; snprintf(top, sizeof(top), "approve?  %lus", (unsigned long)waited);
  gfx.drawString(top, W / 2, cardTop - 30);
  gfx.drawString(top, W / 2 + 1, cardTop - 30);
  gfx.setTextDatum(TL_DATUM);

  // Tool name — largest size whose pixel width fits, truncated with '~' if
  // even the smallest won't (avoids the off-screen wrap that garbled it).
  const int maxW = W - 20;
  int toolSize = 3;
  while (toolSize > 2 && (int)strlen(tama.promptTool) * 6 * toolSize > maxW)
    toolSize--;
  char toolBuf[40];
  int maxChars = maxW / (6 * toolSize);
  if ((int)strlen(tama.promptTool) > maxChars && maxChars >= 2) {
    int keep = maxChars - 1;
    memcpy(toolBuf, tama.promptTool, keep);
    toolBuf[keep] = '~';
    toolBuf[keep + 1] = 0;
  } else {
    snprintf(toolBuf, sizeof(toolBuf), "%s", tama.promptTool);
  }
  gfx.setTextColor(0xFFFF, 0x0000);
  gfx.setTextSize(toolSize);
  gfx.setTextDatum(TL_DATUM);
  gfx.drawString(toolBuf, 10, cardTop + 14);   // left-aligned with the hint

  // Hint — up to 2 lines, wrapped to the width that actually fits.
  gfx.setTextSize(2);
  gfx.setTextColor(0xC618, 0x0000);
  static char hintLines[6][48];
  const int hintCols = (W - 20) / (6 * 2);
  uint8_t hn = wrapInto(tama.promptHint, hintLines, 6, hintCols);
  for (uint8_t i = 0; i < hn && i < 2; i++) {
    gfx.setCursor(10, cardTop + 50 + i * 18);
    gfx.print(hintLines[i]);
  }

  // No DENY/APPROVE labels — swipe right approves, left denies. After a
  // decision, just confirm it was sent.
  if (responseSent) {
    gfx.setTextSize(3);
    gfx.setTextDatum(TC_DATUM);
    gfx.setTextColor(0x8410, 0x0000);
    gfx.drawString("sent...", W / 2, H - 36);
    gfx.setTextDatum(TL_DATUM);
  }

  // Swipe-direction hints: slim glows on both edges the whole time the
  // prompt is up — right green = approve, left red = deny — so it's
  // obvious which way to swipe. Dimmer than the decision flash below
  // (which overdraws them), and dropped once the answer is sent.
  if (!responseSent) {
    static const uint16_t hintG[3] = { 0x03E0, 0x01E0, 0x00C0 };
    static const uint16_t hintR[3] = { 0x7800, 0x3800, 0x1800 };
    const int step = 3;   // brightest band sits at the very edge
    for (int k = 0; k < 3; k++) {
      gfx.fillRect(W - (k + 1) * step, 0, step, H, hintG[k]);
      gfx.fillRect(k * step, 0, step, H, hintR[k]);
    }
  }

  // Swipe feedback: a brief 3-step glow inward from the edge you swiped.
  if (decisionFlashSide != 0 && (int32_t)(millis() - decisionFlashUntil) < 0) {
    static const uint16_t green[3] = { 0x07E0, 0x03E0, 0x01E0 };
    static const uint16_t red[3]   = { 0xF800, 0x7800, 0x3800 };
    const int step = 8;
    for (int k = 0; k < 3; k++) {
      if (decisionFlashSide > 0) gfx.fillRect(W - (k + 1) * step, 0, step, H, green[k]);
      else                       gfx.fillRect(k * step, 0, step, H, red[k]);
    }
  }
}

// ── battery widget / PET / INFO drawing (3i.4) ─────────────────────────────
// The clock widget + full-screen clock face moved to clock.{h,cpp}; the face
// still calls drawBatteryWidget() below (kept non-static for that reason).

// Battery widget: tiny 12×28 vertical icon (tip up) with proportional fill
// rising from the bottom, plus percentage text to the left at body centre.
// Drawn in the top-right corner on the home buddy view and on the clock
// face — the two views where there's empty space to the right of the
// main content. PET / INFO have a page counter there, so they skip it.
//
// Colour signals: yellow = actively charging, green = healthy, orange =
// low, red = critical. A lightning bolt overlays the body whenever USB
// power is present (even at 100%, when the charger state machine idles).
void drawBatteryWidget(int bx, int by) {
  if (!settings().battery) return;   // "battery" setting hides the widget
  int pct = batteryPercent();
  bool chg = charging();

  const int bw = 14, bh = 28;       // body
  const int tipW = 6, tipH = 3;     // little nub on top

  gfx.fillRect(bx + (bw - tipW) / 2, by, tipW, tipH, 0xC618);
  gfx.drawRoundRect(bx, by + tipH, bw, bh, 2, 0xC618);

  int innerX = bx + 2, innerY = by + tipH + 2;
  int innerW = bw - 4, innerH = bh - 4;
  int fillH  = (innerH * pct) / 100;
  if (fillH > 0) {
    uint16_t fc = chg       ? 0xFFE0   // yellow when charging
                : pct >= 70 ? 0x07E0   // green
                : pct >= 30 ? 0xFD20   // orange
                            : 0xF800;  // red
    gfx.fillRect(innerX, innerY + innerH - fillH, innerW, fillH, fc);
  }

  if (onUsb()) {
    // Zigzag bolt centred on the body: a black pass one pixel fatter acts
    // as an outline so the white bolt reads on both the fill and the empty
    // (black) part of the icon.
    int cx = bx + bw / 2, cy = by + tipH + bh / 2;
    gfx.fillTriangle(cx + 4, cy - 9, cx - 4, cy + 2, cx + 2, cy + 2, 0x0000);
    gfx.fillTriangle(cx - 4, cy + 9, cx + 4, cy - 2, cx - 2, cy - 2, 0x0000);
    gfx.fillTriangle(cx + 3, cy - 8, cx - 3, cy + 1, cx + 1, cy + 1, 0xFFFF);
    gfx.fillTriangle(cx - 3, cy + 8, cx + 3, cy - 1, cx - 1, cy - 1, 0xFFFF);
  }

  gfx.setTextSize(2);
  gfx.setTextColor(0xFFFF, 0x0000);
  gfx.setTextDatum(MR_DATUM);
  char b[8]; snprintf(b, sizeof(b), "%d%%", pct);
  gfx.drawString(b, bx - 6, by + tipH + bh / 2);
  gfx.setTextDatum(TL_DATUM);
}

// Header strip shared by PET / INFO pages: title left, page counter right.
static void drawPageHeader(const char* title, uint8_t page, uint8_t pages, uint16_t accent) {
  gfx.fillSprite(0x0000);
  gfx.setTextSize(3);
  gfx.setTextColor(0xFFFF, 0x0000);
  gfx.setCursor(10 + SAFE, 12 + SAFE); gfx.print(title);
  char pn[10]; snprintf(pn, sizeof(pn), "%u/%u", page + 1, pages);
  gfx.setTextDatum(TR_DATUM);
  gfx.setTextColor(accent, 0x0000);
  gfx.drawString(pn, W - 10 - SAFE, 16 + SAFE);
  gfx.setTextDatum(TL_DATUM);
  gfx.drawFastHLine(0, 60, W, 0x4208);
}

// ── PET pages ──────────────────────────────────────────────────────────────
static void drawPetStats() {
  drawPageHeader(petName(), 0, PET_PAGES, 0xF810);
  gfx.setTextSize(2);

  int y = 80;
  auto label = [&](const char* lbl) {
    gfx.setTextColor(0xC618, 0x0000);
    gfx.setCursor(10, y); gfx.print(lbl);
  };

  // Mood: 4 hearts (filled vs hollow).
  uint8_t mood = statsMoodTier();
  label("mood   ");
  for (int i = 0; i < 4; i++) {
    int cx = 120 + i * 30, cy = y + 8;
    uint16_t col = (mood >= 3) ? 0xF800 : (mood >= 2) ? 0xFA20 : 0xC618;
    if (i < mood) {
      gfx.fillCircle(cx - 5, cy, 5, col);
      gfx.fillCircle(cx + 5, cy, 5, col);
      gfx.fillTriangle(cx - 10, cy + 2, cx + 10, cy + 2, cx, cy + 12, col);
    } else {
      gfx.drawCircle(cx - 5, cy, 5, 0x4208);
      gfx.drawCircle(cx + 5, cy, 5, 0x4208);
      gfx.drawLine(cx - 10, cy + 2, cx, cy + 12, 0x4208);
      gfx.drawLine(cx + 10, cy + 2, cx, cy + 12, 0x4208);
    }
  }
  y += 36;

  // Fed: 10 dots (progress toward next level).
  uint8_t fed = statsFedProgress();
  label("fed    ");
  for (int i = 0; i < 10; i++) {
    int cx = 120 + i * 22, cy = y + 8;
    if (i < fed) gfx.fillCircle(cx, cy, 6, 0x07E0);
    else         gfx.drawCircle(cx, cy, 6, 0x4208);
  }
  y += 36;

  // Energy: 5 bars (rest tier).
  uint8_t en = statsEnergyTier();
  label("energy ");
  uint16_t enCol = (en >= 4) ? 0x07FF : (en >= 2) ? 0xFFE0 : 0xFA20;
  for (int i = 0; i < 5; i++) {
    int x = 120 + i * 28, ybar = y;
    if (i < en) gfx.fillRect(x, ybar, 22, 16, enCol);
    else        gfx.drawRect(x, ybar, 22, 16, 0x4208);
  }
  y += 40;

  // Level badge — Claude orange (#DE7643 ≈ RGB565 0xDBA8).
  gfx.fillRoundRect(10, y, 90, 32, 6, 0xDBA8);
  gfx.setTextColor(0xFFFF, 0xDBA8);
  gfx.setCursor(20, y + 8); gfx.printf("Lv %u", stats().level);
  y += 48;

  // Token / approval counters.
  gfx.setTextColor(0xC618, 0x0000);
  auto tokenLine = [&](const char* lbl, uint32_t v) {
    gfx.setCursor(10, y); gfx.print(lbl);
    char b[20];
    if      (v >= 1000000) snprintf(b, sizeof(b), "%lu.%luM", v / 1000000, (v / 100000) % 10);
    else if (v >= 1000)    snprintf(b, sizeof(b), "%lu.%luK", v / 1000,    (v / 100)    % 10);
    else                   snprintf(b, sizeof(b), "%lu",      v);
    gfx.setCursor(180, y); gfx.print(b);
    y += 22;
  };
  tokenLine("tokens   ", stats().tokens);
  tokenLine("today    ", tama.tokensToday);
  y += 6;

  gfx.setCursor(10, y); gfx.printf("approved %u", stats().approvals); y += 22;
  gfx.setCursor(10, y); gfx.printf("denied   %u", stats().denials);   y += 22;
  uint32_t nap = stats().napSeconds;
  gfx.setCursor(10, y);
  gfx.printf("napped   %luh %02lum", nap / 3600, (nap / 60) % 60);
}

static void drawPetHowTo() {
  drawPageHeader(petName(), 1, PET_PAGES, 0xF810);
  gfx.setTextSize(2);
  int y = 80;
  auto ln = [&](uint16_t c, const char* s) {
    gfx.setTextColor(c, 0x0000); gfx.setCursor(10, y); gfx.print(s); y += 22;
  };
  auto gap = [&]() { y += 10; };

  ln(0xF810, "MOOD");
  ln(0xC618, " approve fast = up");
  ln(0xC618, " deny lots = down"); gap();
  ln(0x07E0, "FED");
  ln(0xC618, " 50K tokens = lvl up");
  ln(0xC618, " + confetti"); gap();
  ln(0x07FF, "ENERGY");
  ln(0xC618, " face-down to nap");
  ln(0xC618, " refills to full"); gap();
  ln(0xFFE0, "IDLE 30s = screen off");
  ln(0xC618, " any input wakes it"); gap();
  ln(0xFFFF, "swipe > APPROVE");
  ln(0xFFFF, "swipe < DENY");
}

// ── INFO pages ─────────────────────────────────────────────────────────────
static void drawInfoAbout() {
  drawPageHeader("INFO", 0, INFO_PAGES, 0x07E0);
  gfx.setTextSize(3);
  gfx.setTextColor(0x07E0, 0x0000);
  gfx.setCursor(10, 80); gfx.print("ABOUT");
  gfx.setTextSize(2);
  gfx.setTextColor(0xC618, 0x0000);
  int y = 140;
  const char* about[] = {
    "I watch your Claude",
    "desktop sessions.",
    "",
    "I sleep when nothing's",
    "happening, wake when you",
    "start working, get impatient",
    "when approvals pile up.",
    "",
    "Swipe RIGHT on a prompt",
    "to approve from here.",
    "",
    "18 species. Swipe L/R",
    "on home to change pet.",
  };
  for (auto s : about) { gfx.setCursor(10, y); gfx.print(s); y += 22; }
}

static void drawInfoButtons() {
  drawPageHeader("INFO", 1, INFO_PAGES, 0x07E0);
  gfx.setTextSize(3);
  gfx.setTextColor(0x07E0, 0x0000);
  gfx.setCursor(10, 80); gfx.print("CONTROLS");
  gfx.setTextSize(2);
  int y = 130;
  auto k = [&](uint16_t c, const char* lbl, const char* desc) {
    gfx.setTextColor(c, 0x0000); gfx.setCursor(10, y); gfx.print(lbl);
    gfx.setTextColor(0xC618, 0x0000); gfx.setCursor(160, y); gfx.print(desc);
    y += 22;
  };
  k(0xFFFF, "swipe >",       "approve");
  k(0xFFFF, "swipe <",       "deny");
  k(0xFFFF, "swipe </>",     "change pet");
  k(0xFFFF, "tap HUD",       "open log");
  k(0xFFFF, "swipe v",       "next view");
  k(0xFFFF, "swipe ^",       "prev view");
  y += 10;
  k(0xFFE0, "PWR tap",       "screen off");
  k(0xFFE0, "BOOT tap",      "menu/select");
  k(0xFFE0, "BOOT hold",     "screenshot");
  k(0xFFE0, "PWR hold",      "power off");
}

static void drawInfoClaude() {
  drawPageHeader("INFO", 2, INFO_PAGES, 0x07E0);
  gfx.setTextSize(3);
  gfx.setTextColor(0x07E0, 0x0000);
  gfx.setCursor(10, 80); gfx.print("CLAUDE");
  gfx.setTextSize(2);
  gfx.setTextColor(0xC618, 0x0000);
  int y = 130;
  auto kv = [&](const char* k, const char* v) {
    gfx.setCursor(10, y); gfx.print(k);
    gfx.setCursor(180, y); gfx.print(v);
    y += 22;
  };
  char b[16];
  snprintf(b, sizeof(b), "%u", tama.sessionsTotal);   kv("sessions",  b);
  snprintf(b, sizeof(b), "%u", tama.sessionsRunning); kv("running",   b);
  snprintf(b, sizeof(b), "%u", tama.sessionsWaiting); kv("waiting",   b);
  y += 10;
  gfx.setTextColor(0xFFFF, 0x0000);
  gfx.setCursor(10, y); gfx.print("LINK"); y += 22;
  gfx.setTextColor(0xC618, 0x0000);
  kv("via",  dataScenarioName());
  kv("ble",  bleConnected() ? (bleSecure() ? "encrypted" : "OPEN") : "-");
  uint32_t age = tama.lastUpdated ? (millis() - tama.lastUpdated) / 1000 : 0;
  snprintf(b, sizeof(b), "%lus", (unsigned long)age);
  kv("last msg", b);
}

static void drawInfoDevice() {
  drawPageHeader("INFO", 3, INFO_PAGES, 0x07E0);
  gfx.setTextSize(3);
  gfx.setTextColor(0x07E0, 0x0000);
  gfx.setCursor(10, 80); gfx.print("DEVICE");

  // Battery summary up top: % + state, large.
  int pct = batteryPercent();
  int mV  = batteryMilliVolts();
  bool usb = onUsb();
  bool charge = charging();
  const char* state = !powerOk() ? "?"
                    : charge   ? "charging"
                    : usb      ? "usb"
                    : "battery";
  uint16_t stateCol = charge ? 0xFA20 : (usb ? 0x07E0 : 0xC618);
  gfx.setTextSize(4);
  gfx.setTextColor(0xFFFF, 0x0000);
  gfx.setCursor(10, 130); gfx.printf("%d%%", pct);
  gfx.setTextSize(2);
  gfx.setTextColor(stateCol, 0x0000);
  gfx.setCursor(110, 140); gfx.print(state);

  gfx.setTextColor(0xC618, 0x0000);
  int y = 195;
  auto kv = [&](const char* k, const char* v) {
    gfx.setCursor(10, y); gfx.print(k);
    gfx.setCursor(180, y); gfx.print(v);
    y += 22;
  };
  char b[24];
  snprintf(b, sizeof(b), "%d.%02dV", mV / 1000, (mV % 1000) / 10);
  kv("battery", b);

  uint32_t up = millis() / 1000;
  snprintf(b, sizeof(b), "%luh %02lum", up / 3600, (up / 60) % 60);
  kv("uptime", b);

  snprintf(b, sizeof(b), "%uKB", (unsigned)(ESP.getFreeHeap() / 1024));
  kv("heap", b);

  snprintf(b, sizeof(b), "%u/4", brightLevel);
  kv("bright", b);

  float t = imuTemp();
  if (!isnan(t)) { snprintf(b, sizeof(b), "%dC", (int)t); kv("imu temp", b); }

  // Storage backend + free space. SD reports in MB (gigabyte cards), LFS
  // in KB (a few MB partition).
  uint64_t tot = storageTotalBytes(), used = storageUsedBytes();
  if (tot > 0) {
    uint64_t free = tot - used;
    if (storageBackend() == STORAGE_SD) {
      snprintf(b, sizeof(b), "%s %lluMB", storageBackendName(),
               (unsigned long long)(free / (1024ULL * 1024ULL)));
    } else {
      snprintf(b, sizeof(b), "%s %uKB", storageBackendName(),
               (unsigned)(free / 1024));
    }
  } else {
    snprintf(b, sizeof(b), "%s", storageBackendName());
  }
  kv("storage", b);

  // Last hang recovery, if any — the loop stage the *previous* boot wedged in,
  // recovered by the loop/AXP watchdog. Blank after a clean cold boot. Drawn in
  // alert orange so a field repro stands out without needing serial.
  if (lastHang[0]) {
    gfx.setTextColor(0xFD20, 0x0000);
    kv("recover", lastHang);
    gfx.setTextColor(0xC618, 0x0000);
  }
}

static void drawInfoBluetooth(const char* btName) {
  drawPageHeader("INFO", 4, INFO_PAGES, 0x07E0);
  gfx.setTextSize(3);
  gfx.setTextColor(0x07E0, 0x0000);
  gfx.setCursor(10, 80); gfx.print("BLUETOOTH");
  bool linked = bleConnected();

  gfx.setTextSize(3);
  gfx.setTextColor(linked ? 0x07E0 : 0xFA20, 0x0000);
  gfx.setCursor(10, 130); gfx.print(linked ? "linked" : "discoverable");

  gfx.setTextSize(2);
  gfx.setTextColor(0xC618, 0x0000);
  int y = 180;
  gfx.setCursor(10, y); gfx.print("name"); gfx.setCursor(120, y); gfx.print(btName); y += 22;

  uint8_t mac[6] = {0}; esp_read_mac(mac, ESP_MAC_BT);
  char m[24];
  snprintf(m, sizeof(m), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  gfx.setCursor(10, y); gfx.print("mac"); gfx.setCursor(120, y); gfx.print(m); y += 32;

  if (linked) {
    uint32_t age = tama.lastUpdated ? (millis() - tama.lastUpdated) / 1000 : 0;
    gfx.setCursor(10, y); gfx.printf("last msg  %lus", (unsigned long)age);
  } else {
    gfx.setTextColor(0xFFFF, 0x0000);
    gfx.setCursor(10, y); gfx.print("TO PAIR"); y += 24;
    gfx.setTextColor(0xC618, 0x0000);
    gfx.setCursor(10, y); gfx.print(" Open Claude desktop"); y += 22;
    gfx.setCursor(10, y); gfx.print(" > Developer"); y += 22;
    gfx.setCursor(10, y); gfx.print(" > Hardware Buddy"); y += 22;
  }
}

static void drawInfoCredits() {
  drawPageHeader("INFO", 5, INFO_PAGES, 0x07E0);
  gfx.setTextSize(3);
  gfx.setTextColor(0x07E0, 0x0000);
  gfx.setCursor(10, 80); gfx.print("CREDITS");
  gfx.setTextSize(2);
  gfx.setTextColor(0xC618, 0x0000);
  int y = 140;
  auto ln = [&](uint16_t c, const char* s) {
    gfx.setTextColor(c, 0x0000); gfx.setCursor(10, y); gfx.print(s); y += 22;
  };
  ln(0xC618, "made by");
  ln(0xFFFF, "Felix Rieseberg"); y += 10;
  ln(0xC618, "AMOLED port");
  ln(0xFFFF, "github/tsisar"); y += 10;
  ln(0xC618, "source");
  ln(0xFFFF, "github.com/anthropics");
  ln(0xFFFF, "/claude-desktop-buddy"); y += 10;
  ln(0xC618, "hardware");
  ln(0xFFFF, "Waveshare ESP32-S3");
  ln(0xFFFF, "Touch-AMOLED-1.8");
}

// Dispatch for DISP_INFO based on infoPage.
static void drawInfo(const char* btName) {
  switch (infoPage) {
    case 0: drawInfoAbout();             break;
    case 1: drawInfoButtons();           break;
    case 2: drawInfoClaude();            break;
    case 3: drawInfoDevice();            break;
    case 4: drawInfoBluetooth(btName);   break;
    case 5: drawInfoCredits();           break;
    default: drawInfoAbout();            break;
  }
}

// Dispatch for DISP_PET based on petPage.
static void drawPet() {
  switch (petPage) {
    case 0: drawPetStats(); break;
    case 1: drawPetHowTo(); break;
    default: drawPetStats(); break;
  }
}

// ── passkey pairing screen (3i.5) ──────────────────────────────────────────
//
// NimBLE prompts the host for a 6-digit code. blePasskey() returns it as a
// uint32_t for the duration of the pairing exchange and 0 otherwise. While
// non-zero we replace the entire view so the user can't miss the digits.
static void drawPasskey() {
  gfx.fillSprite(0x0000);
  gfx.setTextDatum(MC_DATUM);

  gfx.setTextSize(3);
  gfx.setTextColor(0x07FF, 0x0000);
  gfx.drawString("BLUETOOTH PAIRING", W / 2, 100);

  gfx.setTextSize(8);
  gfx.setTextColor(0xFFFF, 0x0000);
  char b[8]; snprintf(b, sizeof(b), "%06lu", (unsigned long)blePasskey());
  gfx.drawString(b, W / 2, H / 2);

  gfx.setTextSize(2);
  gfx.setTextColor(0xC618, 0x0000);
  gfx.drawString("enter on desktop", W / 2, H - 100);
  gfx.setTextDatum(TL_DATUM);
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
  "settings", "turn off", "help", "about", "demo", "reboot", "close"
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
      // Blank via the brightness path (screenOn → setBrightness(0)), same as
      // auto-sleep. NOT a rail toggle: ALDO1 is the codec rail and the panel
      // isn't gated by ALDO1/ALDO3 anyway (see docs/device-power-map.md).
      // Full power-off via AXP2101 OFF register is owned by 3i.5.
      screenOn = false;
      applyBrightness();   // blank the panel immediately (double-tap / PWRON wakes)
      enterState(UI_NORMAL);
      break;
    case 2:   // help → CONTROLS info page
      displayMode = DISP_INFO; infoPage = 1;
      enterState(UI_NORMAL);
      break;
    case 3:   // about → ABOUT info page
      displayMode = DISP_INFO; infoPage = 0;
      enterState(UI_NORMAL);
      break;
    case 4:   // demo
      dataSetDemo(!dataDemo());
      break;
    case 5:   // reboot — clean software restart, for catching boot logs on a
      // glitchy session without pulling power. Clear the hang breadcrumb first
      // so this deliberate reboot reports as a cold boot next time, not a false
      // "[hang] warm reset". Beep + a short blank so the press is acknowledged
      // before the chip resets.
      click(600);
      {
        // Report the loop stage we were in when reboot was pressed: if the UI
        // was sluggish-but-alive, this names what was eating the loop. Then
        // clear the breadcrumb so the next boot reads as a clean cold boot
        // (a deliberate reboot must not masquerade as a watchdog "[hang]").
        uint8_t st = (bcStage < sizeof(LS_NAME) / sizeof(LS_NAME[0])) ? bcStage : 0;
        Serial.printf("[reboot] user-requested restart — was at stage=%s loops=%lu\n",
                      LS_NAME[st], (unsigned long)bcLoops);
      }
      bcMagic = 0;
      Serial.flush();
      delay(150);
      esp_restart();
      break;
    case 6:   // close
      enterState(UI_NORMAL);
      break;
  }
}

// ── settings menu (3i.3) ───────────────────────────────────────────────────
static const char* const SETTINGS_ITEMS[] = {
  "brightness", "sound", "transcript", "battery", "screensaver",
  "ascii pet", "reset", "back",
};
static const int SETTINGS_N = sizeof(SETTINGS_ITEMS) / sizeof(SETTINGS_ITEMS[0]);
// brightLevel itself is declared near the top of the file (drawInfoDevice
// reads it); only the apply path lives here with the rest of settings.

static void applyBrightness() {
  // Surface::setBrightness() forwards to the SH8601 panel. 0..255 range.
  // Map our 5 tiers to a comfortable curve (dim but readable to full).
  // Blanked = brightness 0 PLUS panel sleep-in + touch monitor mode:
  // brightness 0 alone left the SH8601 controller, gate drivers and QSPI
  // interface fully powered and the FT3168 in full active scan — the "off"
  // state battery life depends on cost nearly as much as "on". The parked
  // clock face (aodActive) dims to the lowest tier so days on USB don't
  // burn the digits into the panel.
  static const uint8_t MAP[5] = { 40, 80, 130, 180, 230 };
  if (!dispOk) return;
  if (screenOn) {
    if (panelAsleep) { gfx.displayOn(); panelAsleep = false; }
    gfx.setBrightness(aodActive ? MAP[0] : MAP[brightLevel]);
    touchSetLowPower(false);
  } else {
    gfx.setBrightness(0);
    if (!panelAsleep) { gfx.displayOff(); panelAsleep = true; }
    touchSetLowPower(true);
  }
}

// Visual "shutter" confirmation for screenshotSave() — the beeper can be
// inaudible (muted device, noisy room), so blink the whole panel for
// ~90 ms: white = saved, red = failed. Runs AFTER the BMP is written, so
// the flash itself never lands in the capture, and the next render pass
// repaints the canvas anyway. Brightness is forced up for the blink and
// applyBrightness() restores the screenOn-aware level — meaning the flash
// is visible even on a blanked panel (cmd:shot with the screen off).
// Called from the BOOT-hold handler below and xfer.h's cmd:shot.
static uint32_t shotFlashUntil = 0;   // non-zero while the blink is showing

void shotFlash(bool ok) {
  if (!dispOk) return;
  // cmd:shot can land on a blanked panel — wake it for the blink; the
  // render path re-applies brightness (and re-sleeps the panel) when the
  // deadline below passes, so the loop never sits in a delay() here.
  if (panelAsleep) { gfx.displayOn(); panelAsleep = false; }
  gfx.fillSprite(ok ? 0xFFFF : 0xF800);
  gfx.pushSprite();
  gfx.setBrightness(230);
  shotFlashUntil = millis() + 90;
}

static void drawSettingsMenu() {
  drawModal("SETTINGS", SETTINGS_N, 0xFFE0);
  Settings& s = settings();
  for (int i = 0; i < SETTINGS_N; i++) {
    const char* value = nullptr;
    char buf[16];
    bool boolish = false;
    bool boolval = false;
    switch (i) {
      case 0: snprintf(buf, sizeof(buf), "%u/4", brightLevel);   value = buf; break;
      case 1: boolish = true; boolval = s.sound; break;
      case 2: boolish = true; boolval = s.hud;   break;
      case 3: boolish = true; boolval = s.battery; break;
      case 4: boolish = true; boolval = s.screensaver; break;
      case 5: {
        // tri-state cycle: ASCII species 0..N-1 → GIF (if installed) → 0
        uint8_t total = buddySpeciesCount() + (gifAvailable ? 1 : 0);
        uint8_t pos   = buddyMode ? buddySpeciesIdx() + 1 : total;
        snprintf(buf, sizeof(buf), "%u/%u", pos, total);
        value = buf;
        break;
      }
      // reset and back have no value column
    }
    uint16_t valueCol = 0x07E0;
    if (boolish) {
      value = boolval ? "on" : "off";
      valueCol = boolval ? 0x07E0 : 0x4208;
    }
    drawMenuRow(i, SETTINGS_N, SETTINGS_ITEMS[i], value, 0xFFFF, valueCol);
  }
}

// Cycle the active character through the tri-state order used everywhere:
// ASCII species 0..N-1, then the GIF character as a final slot when one is
// installed. dir = +1 → next, -1 → previous; both wrap around. Persists the
// choice so it survives a reboot (0xFF is the "boot into GIF" sentinel).
static void cycleSpecies(int dir) {
  uint8_t n     = buddySpeciesCount();
  uint8_t total = n + (gifAvailable ? 1 : 0);
  if (total == 0) return;
  uint8_t pos = buddyMode ? buddySpeciesIdx() : n;   // slot n == the GIF
  pos = (uint8_t)((pos + total + dir) % total);
  if (pos < n) {
    buddyMode = true;
    buddySetSpeciesIdx(pos);
    speciesIdxSave(pos);
  } else {
    buddyMode = false;             // landed on the GIF slot
    speciesIdxSave(0xFF);
  }
}

static void applySettings(int idx) {
  Settings& s = settings();
  switch (idx) {
    case 0: brightLevel = (brightLevel + 1) % 5; applyBrightness();
            brightnessSave(brightLevel); return;
    case 1: s.sound   = !s.sound;   break;
    case 2: s.hud     = !s.hud;     break;
    case 3: s.battery = !s.battery; break;
    case 4: s.screensaver = !s.screensaver; break;
    case 5:
      // tri-state cycle: ASCII species 0..N-1 → GIF (if installed) → 0
      cycleSpecies(+1);
      return;
    case 6:   // reset → open reset sub-menu
      enterState(UI_MENU_RESET);
      return;
    case 7:   // back → main
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
    // NVS namespace wipe + BLE bonds + character pack. Restart so all
    // state reloads from defaults.
    xferDeleteAll();
    Preferences p;
    p.begin("buddy", false);
    p.clear();
    p.end();
    bleClearBonds();
    // Deliberate restart — clear the hang breadcrumb so the next boot
    // doesn't log a false "[hang] warm reset" (same as the reboot menu).
    bcMagic = 0;
    delay(300);
    ESP.restart();   // does not return
  }
  if (confirmAction == CONF_DELETE_CHAR) {
    xferDeleteAll();
    Serial.println("[3i.3] character wiped");
  }
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
  // Mirror the open gesture: swipe-up opened the menu, swipe-up closes it
  // — but it bails out of the whole modal stack in one go, not step-by-
  // step. Confirm dialog treats the dismiss as Cancel.
  if (ev.kind == GESTURE_SWIPE_UP) {
    if (uiState == UI_CONFIRM) confirmAction = CONF_NONE;
    enterState(UI_NORMAL);
    click(600);
    return;
  }
  if (ev.kind == GESTURE_SWIPE_DOWN) { stepBack(); return; }

  if (ev.kind != GESTURE_TAP) return;

  if (uiState == UI_CONFIRM) {
    int h = confirmHit(ev.x, ev.y);
    if (h == 0 || h == -2) { doConfirm(0); click(600); }
    else if (h == 1)       { doConfirm(1); click(2400); }
    return;
  }

  int idx = menuHit(ev.x, ev.y, currentMenuItems());
  if (idx == -2) {
    enterState(uiState == UI_MENU_MAIN     ? UI_NORMAL
              : uiState == UI_MENU_SETTINGS ? UI_MENU_MAIN
              :                               UI_MENU_SETTINGS);
    click(600);
    return;
  }
  if (idx < 0) return;

  click(2400);
  switch (uiState) {
    case UI_MENU_MAIN:     applyMainMenu(idx); break;
    case UI_MENU_SETTINGS: applySettings(idx); break;
    case UI_MENU_RESET:    applyReset(idx);    break;
    default: break;
  }
}

// ── physical buttons (3i.3) ────────────────────────────────────────────────
//
// BOOT (GPIO0, active-low, internal pull-up): short tap opens the menu from
// home and activates the highlighted item inside a modal; long press
// (≥600 ms) saves a screenshot from any state (screenshot.h). PWRON IRQ on
// the AXP2101 is the cursor — short press wraps the highlight down through
// the items; a long PWRON hold still hits the chip's hardware power-off
// path, untouched.
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

static void navDown() {
  int n = currentMenuItems();
  if (uiState == UI_CONFIRM) { navIdx ^= 1; return; }
  if (n <= 0) return;
  navIdx = (navIdx + 1) % n;
}
static void navActivate() {
  if (uiState == UI_CONFIRM) {
    if (navIdx == 0) { doConfirm(0); click(600); }
    else             { doConfirm(1); click(2400); }
    return;
  }
  int n = currentMenuItems();
  if (n <= 0 || navIdx < 0 || navIdx >= n) return;
  click(2400);
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
  click(2400);
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
  click(600);
}

// ── setup / loop ───────────────────────────────────────────────────────────
void setup() {
  // Bump the USB-CDC RX buffer before begin(). Default ~256B overruns on
  // long xfer chunk lines (~270B base64 + JSON envelope) — the second
  // chunk silently disappears and the device looks dead. 4KB gives ~15
  // chunks of headroom for the worst-case burst.
  Serial.setRxBufferSize(4096);
  Serial.begin(115200); Serial.setTxTimeoutMs(0); delay(300);
  Serial.println("\n[3i.3] UI shell + menu stack");

  // Hang breadcrumb: if RTC RAM survived (warm reset from the loop-WDT), the
  // last loop stage tells us which subsystem wedged. A cold boot (power-cut,
  // first flash, or the AXP hard-WDT) shows garbage magic → report nothing.
  esp_reset_reason_t rr = esp_reset_reason();
  if (bcMagic == BC_MAGIC) {
    uint8_t st = (bcStage < sizeof(LS_NAME) / sizeof(LS_NAME[0])) ? bcStage : 0;
    snprintf(lastHang, sizeof(lastHang), "%s @%lu", LS_NAME[st],
             (unsigned long)bcLoops);
    Serial.printf("[hang] warm reset (reason=%d) — last stage=%s loops=%lu\n",
                  (int)rr, LS_NAME[st], (unsigned long)bcLoops);
  } else {
    Serial.printf("[hang] cold boot (reason=%d)\n", (int)rr);
  }
  bcMagic = BC_MAGIC; bcStage = 0; bcLoops = 0;

  Wire.begin(IIC_SDA, IIC_SCL, 400000);
  Wire.setTimeOut(50);   // bound any I2C stall (a wedged touch chip must not
                         // hang the loop or starve AXP battery/PWRON reads)
  powerInit(Wire);
  dispOk = gfx.begin();
  imuInit(Wire);
  rtcInit(Wire);
  // If a backup cell kept the PCF85063 running, its time is trustworthy at
  // boot — trust it immediately rather than waiting for the bridge to re-sync.
  // rtcGetDate/Time return false when the oscillator stopped (the OS flag, set
  // on power loss with no backup), so this is a no-op on un-backed boots. The
  // bridge still overwrites with authoritative time + tz once it connects.
  {
    RtcDate rd; RtcTime rt;
    if (rtcOk() && rtcGetDate(&rd) && rtcGetTime(&rt) && rd.Year >= 2024) {
      dataSetRtcValid(true);
      Serial.printf("[3i.4] rtc held time across boot: %04u-%02u-%02u %02u:%02u\n",
                    rd.Year, rd.Month, rd.Date, rt.Hours, rt.Minutes);
    }
  }
  audioInit(Wire);
  touchInit(Wire);
  storageInit();   // SD if available, else LittleFS — for xfer.h (Stage 4c)
  pinMode(BOOT_BTN, INPUT_PULLUP);   // physical menu cursor (3i.3)

  statsLoad();
  settingsLoad();
  brightLevel = brightnessLoad();   // restore saved brightness tier (was always max)
  petNameLoad();
  buddyInit();   // pulls saved species index from NVS (defaults to 0)
  // Scan /characters/ for an installed pack. If one's there, mark it
  // available; if the saved species was the 0xFF sentinel, boot into GIF
  // mode straight away. Otherwise stay on the ASCII species the user
  // last picked.
  if (characterInit(nullptr)) {
    gifAvailable = true;
    uint8_t saved = speciesIdxLoad();
    if (saved == 0xFF) buddyMode = false;
  }
  applyBrightness();
  lastInteractMs = millis();   // arm the auto-screen-off countdown from boot

  uint8_t mac[6] = {0}; esp_read_mac(mac, ESP_MAC_BT);
  snprintf(btName, sizeof(btName), "Claude-%02X%02X", mac[4], mac[5]);
  bleInit(btName);

  // Second-rung watchdog: subscribe loop() to the ESP32 task WDT (~5s). The
  // core auto-feeds it on each loop() return, so a healthy loop never trips
  // it; a software stall the AXP2101 WDT can't see (warm hangs where the chip
  // is still ACKed) gets a panic-reboot here. The AXP2101 hardware WDT armed
  // in powerInit() remains the primary recovery for a fully wedged board.
  enableLoopWDT();

  Serial.printf("[3i.4] disp=%d touch=%d imu=%d rtc=%d pmu=%d  name=%s\n",
                dispOk, touchOk(), imuOk(), rtcOk(), powerOk(), btName);
  Serial.printf("[3i.4] audio ok=%d  sound=%d\n", audioOk(), settings().sound);
  // RTC backup-cell charger (AXP2101 VBACKUP, pin 27). Only the enable bit is
  // observable — there's no ADC for that cell. On stock boards the backup
  // connector (H3 / VBAT2) is unpopulated, so this charges nothing and the
  // clock resets on a full power-off; solder a cell to H3 to keep time.
  Serial.printf("[3i.4] rtc-backup charge=%d (target 3000mV, needs cell on H3)\n",
                batteryBackupChargeEnabled());
}

void loop() {
  uint32_t now = millis();

  // Pet both watchdogs every pass. The AXP2101 hardware WDT (8s) is the real
  // backstop — it power-cycles the board if the loop wedges in a way the
  // ESP32 loop-WDT can't catch (a yielding FreeRTOS wait keeps the IDLE task,
  // and thus the software WDT, alive). The loop-WDT is auto-fed by the core
  // on each loop() return; this only needs to feed the chip-side one.
  powerFeedWatchdog();
  bcLoops++;

  // ── backend pump ──
  bc(LS_DATAPOLL);
  dataPoll(&tama);
  if (statsPollLevelUp()) triggerOneShot(P_CELEBRATE, 3000);

  // The desktop never sends "completed" — a finished turn shows up as
  // running dropping back to 0 (msg becomes "done (success), N turns").
  // Detect that edge and celebrate it.
  static uint8_t prevRunning = 0;
  if (tama.connected && prevRunning > 0 && tama.sessionsRunning == 0)
    triggerOneShot(P_CELEBRATE, 3000);
  prevRunning = tama.sessionsRunning;

  uint8_t base = derive(tama);
  if ((int32_t)(now - oneShotUntil) >= 0) activeState = base;

  if (strcmp(tama.promptId, lastPromptId) != 0) {
    strncpy(lastPromptId, tama.promptId, sizeof(lastPromptId)-1);
    lastPromptId[sizeof(lastPromptId)-1] = 0;
    responseSent = false;
    if (tama.promptId[0]) {
      promptArrivedMs = now;
      click(1200);
      wake();   // new prompt is itself an interaction event
      Serial.printf("[3i.5] PROMPT %s  tool=%s\n", tama.promptId, tama.promptTool);
    }
  }

  // Hold P_SLEEP for 12s after waking so the wake-up animation has time to
  // play. Urgent states (ATTENTION / CELEBRATE / BUSY) still override via
  // the derive() result above.
  if (activeState == P_IDLE && (int32_t)(now - wakeTransitionMs) < 0) {
    activeState = P_SLEEP;
  }

  // ── IMU polling: shake → DIZZY, face-down → nap (3i.5) ──
  bc(LS_IMU);
  if (now - lastShakeCheckMs > 50) {
    lastShakeCheckMs = now;
    float ax = 0, ay = 0, az = 0;
    if (imuGetAccel(&ax, &ay, &az)) {
      // Shake: only fire on home, screen lit, no modal, no oneShot running.
      // Same delta+EMA detector as the M5 build (delta > 0.8g).
      bool shakeEligible = (uiState == UI_NORMAL) && screenOn && !napping
                        && (int32_t)(now - oneShotUntil) >= 0;
      float mag = sqrtf(ax*ax + ay*ay + az*az);
      float delta = fabsf(mag - accelBaseline);
      accelBaseline = accelBaseline * 0.95f + mag * 0.05f;
      if (shakeEligible && delta > 0.8f) {
        triggerOneShot(P_DIZZY, 2000);
        wake();
      }

      // Face-down nap: az < -0.7g with the other axes calm. Debounced
      // 15-frame enter / 8-frame exit so a brief toss doesn't trigger.
      // Skipped during prompt — user is reading it, not napping the device.
      bool inPromptNow = tama.promptId[0] && !responseSent;
      if (!inPromptNow) {
        bool down = (az < -0.7f) && fabsf(ax) < 0.4f && fabsf(ay) < 0.4f;
        if (down) { if (faceDownFrames < 20) faceDownFrames++; }
        else      { if (faceDownFrames > -10) faceDownFrames--; }
        if (!napping && faceDownFrames >= 15) {
          napping = true;
          napStartMs = now;
          screenOn = false;
          applyBrightness();
          Serial.println("[3i.5] nap start");
        } else if (napping && faceDownFrames <= -8) {
          napping = false;
          uint32_t napS = (now - napStartMs) / 1000;
          statsOnNapEnd(napS);
          statsOnWake();
          wake();
          Serial.printf("[3i.5] nap end (%lus)\n", (unsigned long)napS);
        }
      }
    }
  }

  // ── auto screen-off after idle (3i.5) ──
  // Skipped during prompt (the user is mid-decision). Parked & clockable
  // means the screensaver face would take over here — gated by the
  // screensaver setting. Recomputed at render time too (display state can
  // change via gestures in between), so keep it side-effect free.
  bc(LS_AUTOOFF);
  bool inPromptNow = tama.promptId[0] && !responseSent;
  auto parkedClockable = [&]() {
    return (uiState == UI_NORMAL) && (displayMode == DISP_NORMAL)
        && !inPromptNow && !responseSent
        && tama.sessionsRunning == 0 && tama.sessionsWaiting == 0
        && dataRtcValid() && settings().screensaver;
  };
  // On USB the clock face wants to stay visible, so never auto-off there.
  // On battery the panel turns off after IDLE_MS — but if the screensaver
  // is on, the clock face first gets BATTERY_CLOCK_MS of screen time before
  // the panel sleeps.
  uint32_t offAt = parkedClockable() ? IDLE_MS + BATTERY_CLOCK_MS : IDLE_MS;
  if (screenOn && !napping && !inPromptNow && !onUsb()
      && (now - lastInteractMs > offAt)) {
    screenOn = false;
    applyBrightness();
    Serial.println("[3i.5] auto screen-off");
  }

  // ── BLE passkey arrival: wake and beep (3i.5) ──
  bc(LS_BLE);
  uint32_t pk = blePasskey();
  if (pk && !lastPasskey) {
    wake();
    click(1800);
    Serial.printf("[3i.5] passkey %06lu\n", (unsigned long)pk);
  }
  lastPasskey = pk;

  // ── physical buttons (parallel to touch) ──
  bc(LS_BUTTONS);
  BootEvent be = pollBoot();
  PwronEvent pw = powerPollButton();
  if (pw != PWRON_NONE || be != BOOT_NONE) {
    VLOG("[3i.5] btn  BOOT=%d PWRON=%d  ui=%d disp=%d\n",
         (int)be, (int)pw, (int)uiState, (int)displayMode);
    // Any physical press counts as interaction — wake the screen and arm
    // the 30s timeout. PWRON-short's own screen-toggle path still gets to
    // flip screenOn afterwards (toggle wins; wake() doesn't force-on).
    lastInteractMs = now;
    if (!screenOn && pw != PWRON_SHORT) {
      // Skip auto-wake when PWRON_SHORT is the event — that case is
      // handled below where the user might be intending screen-off.
      wake();
    }
  }

  // BOOT hold = screenshot from ANY state — home, menus, modals. One
  // gesture, one meaning, so the buttons never need per-mode re-learning.
  if (be == BOOT_LONG) {
    bool ok = screenshotSave();
    shotFlash(ok);              // white blink = saved, red = failed
    if (ok) click(2000);        // camera-shutter tick on success
    else    beep(400, 200);     // low buzz on failure (no card / write error)
  }

  if (uiState != UI_NORMAL) {
    if      (be == BOOT_SHORT)  { navActivate(); }
    else if (pw == PWRON_SHORT) { navDown(); click(1800); }
    // PWRON_LONG falls through — AXP2101 owns it (hardware power-off).
  } else {
    // No modal up. Physical keys no longer flip views/pages — that's touch
    // only now (the swipe block below). Each key does exactly one thing here:
    //   • PWRON short → blank/toggle the screen, from any home view
    //   • BOOT  short → open the menu
    if (pw == PWRON_SHORT) {
      screenOn = !screenOn;
      applyBrightness();
      if (screenOn) nextDraw = 0;   // relight shows a fresh frame at once
    }
    if (be == BOOT_SHORT)  { enterState(UI_MENU_MAIN); click(800); }
  }

  // ── touch dispatch ──
  bc(LS_TOUCH);
  // While blanked the FT3168 sits in monitor mode and only a double-tap
  // wakes — poll at 50 ms instead of every ~8 ms pass (was ~250 I2C
  // transactions/s against a sleeping panel).
  static uint32_t lastBlankPollMs = 0;
  if (screenOn || now - lastBlankPollMs >= 50) {
    lastBlankPollMs = now;
    gestureUpdate();
  }
  GestureEvent ev = gestureGet();
  bool inPrompt = tama.promptId[0] && !responseSent;

  // While blanked, a single stray touch must NOT wake the screen — only a
  // deliberate double-tap does. Detect it here and consume the event so the
  // normal dispatch below sees nothing (no UI action leaks through on wake).
  if (ev.kind != GESTURE_NONE && !screenOn) {
    if (ev.kind == GESTURE_TAP) {
      if (now - lastWakeTapMs <= DOUBLE_TAP_MS) { lastWakeTapMs = 0; wake(); }
      else lastWakeTapMs = now;
    }
    ev.kind = GESTURE_NONE;   // swallow everything while off
  }

  if (ev.kind != GESTURE_NONE) {
    VLOG("[3i.5] gesture %u at (%u,%u) d(%d,%d) state=%d\n",
         (unsigned)ev.kind, ev.x, ev.y, ev.dx, ev.dy, uiState);
    wake();   // any gesture counts as interaction
    if (uiState != UI_NORMAL) {
      handleModalGesture(ev);
    } else if (inPrompt) {
      if (ev.kind == GESTURE_SWIPE_RIGHT) {
        decisionFlashSide = 1; decisionFlashUntil = now + 600; mockApprove();
      } else if (ev.kind == GESTURE_SWIPE_LEFT) {
        decisionFlashSide = -1; decisionFlashUntil = now + 600; mockDeny();
      }
    } else if (displayMode == DISP_NORMAL) {
      // Up/down cycle the home views (down = forward, up = back). The menu
      // opens on BOOT short (see the button block above).
      if      (ev.kind == GESTURE_SWIPE_DOWN) {
        displayMode = DISP_PET; petPage = 0; clickSoft(700);
      } else if (ev.kind == GESTURE_SWIPE_UP) {
        displayMode = DISP_INFO; infoPage = 0; clickSoft(700);
      } else if (ev.kind == GESTURE_SWIPE_RIGHT) {
        // Home: swipe through characters. Right = next, left = previous.
        cycleSpecies(+1); clickSoft(700);
      } else if (ev.kind == GESTURE_SWIPE_LEFT) {
        cycleSpecies(-1); clickSoft(700);
      } else if (ev.kind == GESTURE_TAP && ev.y >= HUD_TOP) {
        // Tap the transcript strip → open the full-screen log.
        displayMode = DISP_TRANSCRIPT; tScroll = 0; clickSoft(700);
      }
    } else if (displayMode == DISP_TRANSCRIPT) {
      // Full-screen log: swipe up (or tap) closes — same as dismissing a
      // menu. Swipe down scrolls back into history (older lines). Reopening
      // always lands back at the live tail.
      const uint8_t step = (TRANS_ROWS > 1) ? TRANS_ROWS - 1 : 1;
      if      (ev.kind == GESTURE_SWIPE_UP || ev.kind == GESTURE_TAP) {
        displayMode = DISP_NORMAL; clickSoft(700);
      } else if (ev.kind == GESTURE_SWIPE_DOWN) {
        tScroll += step; clickSoft(700);
      }
    } else if (displayMode == DISP_PET) {
      if      (ev.kind == GESTURE_SWIPE_DOWN) {
        displayMode = DISP_INFO; infoPage = 0; clickSoft(700);
      } else if (ev.kind == GESTURE_SWIPE_UP) {
        displayMode = DISP_NORMAL; clickSoft(700);
      } else if (ev.kind == GESTURE_SWIPE_LEFT) {
        // Book convention: drag finger right-to-left to reveal next page.
        petPage = (petPage + 1) % PET_PAGES; clickSoft(700);
      } else if (ev.kind == GESTURE_SWIPE_RIGHT) {
        petPage = (petPage + PET_PAGES - 1) % PET_PAGES; clickSoft(700);
      }
    } else if (displayMode == DISP_INFO) {
      if      (ev.kind == GESTURE_SWIPE_DOWN) {
        displayMode = DISP_NORMAL; clickSoft(700);
      } else if (ev.kind == GESTURE_SWIPE_UP) {
        displayMode = DISP_PET; petPage = 0; clickSoft(700);
      } else if (ev.kind == GESTURE_SWIPE_LEFT) {
        infoPage = (infoPage + 1) % INFO_PAGES; clickSoft(700);
      } else if (ev.kind == GESTURE_SWIPE_RIGHT) {
        infoPage = (infoPage + INFO_PAGES - 1) % INFO_PAGES; clickSoft(700);
      }
    }
  }

  // ── low-battery stats flush ──
  // The AXP2101 hard-cuts power at ~5% SoC; token progress accumulated in
  // RAM (it persists on level-up only) would vanish with it. One-shot
  // flush at 15% on battery; re-arms after a recharge or on USB.
  static bool lowBattFlushed = false;
  static uint32_t lastBattCheckMs = 0;
  if (now - lastBattCheckMs >= 5000) {
    lastBattCheckMs = now;
    if (powerOk()) {
      int pct = batteryPercent();
      if (!onUsb() && pct >= 0 && pct <= 15) {
        if (!lowBattFlushed) {
          lowBattFlushed = true;
          statsMarkDirty();
          statsSave();
          Serial.printf("[pwr] low battery (%d%%) — stats flushed\n", pct);
        }
      } else if (onUsb() || pct > 20) {
        lowBattFlushed = false;
      }
    }
  }

  // ── CPU clock follows the panel ──
  // 240 MHz lit or mid-transfer, 80 MHz blanked. BLE, I2S and USB-CDC run
  // from APB/PLL clocks the divider doesn't touch, and nothing the blanked
  // loop does (touch/PMU polls, JSON heartbeats) needs 240 MHz.
  static bool cpuFast = true;
  bool wantFast = screenOn || xferActive();
  if (wantFast != cpuFast) {
    cpuFast = wantFast;
    setCpuFrequencyMhz(wantFast ? 240 : 80);
  }

  // ── render ──
  // Screenshot blink in progress: hold the white/red frame on the glass
  // (no new flush overwrites it), then restore brightness — and re-sleep
  // the panel if the screen is meant to be off — once the deadline passes.
  if (shotFlashUntil) {
    if ((int32_t)(now - shotFlashUntil) < 0) { delay(8); return; }
    shotFlashUntil = 0;
    applyBrightness();
    nextDraw = 0;
  }
  // Blanked panel: skip the whole compose + 322 KB QSPI flush — it used to
  // run at full pace (up to 25 Hz in SVG mode) into a sleeping panel, and
  // the blanked state is exactly the one battery life depends on. All the
  // wake paths (data pump, IMU, buttons, double-tap) ran above; xfer keeps
  // the fast tick so a host push arriving while blanked isn't throttled.
  if (!screenOn) { delay(xferActive() ? 8 : 40); return; }
  if (!dispOk || now < nextDraw) { delay(8); return; }
  // SVG characters are time-indexed: every layer maps the shared cycle
  // onto its own frame count, so frame holds aren't multiples of any one
  // fixed cadence and polling aliases against them — layers visibly step
  // at uneven moments relative to each other. Instead of a grid, render
  // event-driven: svgAnimNextEventMs() says exactly when the next layer
  // flips a frame, so every switch lands on time (within the loop's ~8 ms
  // sleep) for ANY svgDuration, and we redraw only when something changes.
  // Capped at 200 ms so the HUD/battery and menu latency keep the stock
  // pace between distant frame events.
  if (svgAnimLoaded()) {
    uint32_t e = svgAnimNextEventMs();
    // Floor of 40 ms: when two layers' switches land almost together
    // (deltas as low as 18 ms exist in the claude-svg schedule), draw
    // them in one pass instead of back-to-back full-canvas QSPI pushes —
    // invisible to the eye, kinder to the shared display/touch 3.3V rail.
    if (e < 40)  e = 40;
    nextDraw = now + (e < 200 ? e : 200);
  } else {
    nextDraw = now + 200;
  }
  bc(LS_RENDER);

  // Clock face takes over the home view when the device is parked: RTC
  // synced, no live work, no prompt, no menu, in DISP_NORMAL — and only
  // after IDLE_MS of no interaction. settings().screensaver gates it (off ⇒
  // same as an unset RTC: on USB the home view stays up, on battery the
  // panel sleeps via the auto-screen-off path above). On USB the face stays
  // up indefinitely; on battery it shows for BATTERY_CLOCK_MS, then the
  // auto-screen-off above blanks the panel (and this goes false in step).
  bool clocking = parkedClockable()
               && (now - lastInteractMs > IDLE_MS)
               && (onUsb() || (now - lastInteractMs <= IDLE_MS + BATTERY_CLOCK_MS));

  // Parked face = AOD: dim to the lowest tier while it shows (burn-in
  // protection for the days-long USB park), restore on any other view.
  if (clocking != aodActive) {
    aodActive = clocking;
    applyBrightness();
  }

  // Clock-face dirty skip: the parked face only changes once per second
  // (charge-state flips on the widget lag <=1 s — acceptable), yet it was
  // recomposed and flushed at the full cadence — 4 of every 5 pushes were
  // pixel-identical 322 KB QSPI transfers on the view the device spends
  // most of its plugged-in life in. 0xFF = cache invalid (previous frame
  // wasn't the face), so entry/exit always forces a full draw.
  static uint8_t prevClockSec = 0xFF;
  if (clocking) {
    RtcTime ct;
    if (rtcGetTime(&ct)) {
      if (ct.Seconds == prevClockSec) { delay(8); return; }
      prevClockSec = ct.Seconds;
    } else {
      prevClockSec = 0xFF;
    }
  } else {
    prevClockSec = 0xFF;
  }

  // Passkey takes priority over everything except an actual approval
  // prompt — the user has 30s to type the code into the desktop.
  if      (inPrompt || responseSent) drawApproval();
  else if (blePasskey())             drawPasskey();
  else if (xferActive())             drawXferProgress();
  else if (clocking)                 clockDrawFace();
  else if (displayMode == DISP_TRANSCRIPT) drawTranscript();
  else if (displayMode == DISP_PET)  drawPet();
  else if (displayMode == DISP_INFO) drawInfo(btName);
  else                               drawHome();
  // Battery widget is drawn locally inside clockDrawFace() and drawPet() —
  // see those functions. Home / INFO / approval / passkey skip it.

  switch (uiState) {
    case UI_MENU_MAIN:     drawMainMenu();     break;
    case UI_MENU_SETTINGS: drawSettingsMenu(); break;
    case UI_MENU_RESET:    drawResetMenu();    break;
    case UI_CONFIRM:       drawConfirmModal(); break;
    default: break;
  }

  bc(LS_PUSH);
  gfx.pushSprite();
  delay(8);
}
