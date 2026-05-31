// Stage 3c — all 18 REAL species, full-screen on AMOLED.
//
// Unlike 3b (which copied the cat art), this compiles the actual
// src/buddies/*.cpp files and renders them through Surface. It provides the
// buddy_common.h symbols (the BUDDY_* constants + the print helpers) backed
// by Surface instead of TFT_eSprite — the AMOLED counterpart of the M5
// implementations in src/buddy.cpp.
//
// Tap left/right half of the screen → previous/next species. State auto-
// cycles every 3s. APPROVE/DENY buttons retained.
//
// Selected by build_src_filter: +<hal/> +<buddies/> +<stage3c_amoled.cpp>

#include <Arduino.h>
#include <Wire.h>
#include <string.h>
#include "board_pins.h"
#include "hal/display.h"
#include "buddy.h"
#include "buddy_common.h"

static Surface gfx;
static bool dispOk = false, touchOk = false;
static const int W = LCD_WIDTH, H = LCD_HEIGHT;

// ── buddy_common.h symbol definitions (AMOLED / Surface backing) ──
// Geometry: these are the 1x "logical" coords the species art is authored
// in. BUDDY_X_CENTER is both the pixel center of the screen AND the anchor
// the helpers scale horizontal offsets around (matches src/buddy.cpp).
const int BUDDY_X_CENTER = W / 2;   // 184
const int BUDDY_CANVAS_W = W;       // 368
const int BUDDY_Y_BASE   = 30;      // 1x; *SCALE in buddyPrintSprite
const int BUDDY_Y_OVERLAY= 6;       // 1x; overlay particles sit above body
const int BUDDY_CHAR_W   = 6;
const int BUDDY_CHAR_H   = 8;

const uint16_t BUDDY_BG     = 0x0000;
const uint16_t BUDDY_HEART  = 0xF810;
const uint16_t BUDDY_DIM    = 0x8410;
const uint16_t BUDDY_YEL    = 0xFFE0;
const uint16_t BUDDY_WHITE  = 0xFFFF;
const uint16_t BUDDY_CYAN   = 0x07FF;
const uint16_t BUDDY_GREEN  = 0x07E0;
const uint16_t BUDDY_PURPLE = 0xA01F;
const uint16_t BUDDY_RED    = 0xF800;
const uint16_t BUDDY_BLUE   = 0x041F;

static const uint8_t SCALE = 4;     // glyph 24x32 at text size 4

// Print helpers — same math as src/buddy.cpp but fixed SCALE, Surface target,
// and NO per-line space trimming (trimming desyncs the 12-col grid; see the
// stage-3b alignment fix).
void buddyPrintLine(const char* line, int yPx, uint16_t color, int xOff) {
  int len = strlen(line);
  int w = len * BUDDY_CHAR_W * SCALE;
  int x = BUDDY_X_CENTER - w / 2 + xOff * SCALE;
  gfx.setTextColor(color, BUDDY_BG);
  gfx.setCursor(x, yPx);
  gfx.print(line);
}
void buddyPrintSprite(const char* const* lines, uint8_t nLines, int yOffset,
                      uint16_t color, int xOff) {
  gfx.setTextSize(SCALE);
  int yBase = BUDDY_Y_BASE * SCALE - (SCALE - 1) * 14;
  for (uint8_t i = 0; i < nLines; i++)
    buddyPrintLine(lines[i], yBase + (yOffset + i * BUDDY_CHAR_H) * SCALE, color, xOff);
}
void buddySetCursor(int x, int y) {
  gfx.setCursor(BUDDY_X_CENTER + (x - BUDDY_X_CENTER) * SCALE, y * SCALE);
}
void buddySetColor(uint16_t fg) { gfx.setTextColor(fg, BUDDY_BG); }
void buddyPrint(const char* s)  { gfx.setTextSize(SCALE); gfx.print(s); }

// ── species registry (same list as src/buddy.cpp) ──
extern const Species CAPYBARA_SPECIES, DUCK_SPECIES, GOOSE_SPECIES, BLOB_SPECIES,
  CAT_SPECIES, DRAGON_SPECIES, OCTOPUS_SPECIES, OWL_SPECIES, PENGUIN_SPECIES,
  TURTLE_SPECIES, SNAIL_SPECIES, GHOST_SPECIES, AXOLOTL_SPECIES, CACTUS_SPECIES,
  ROBOT_SPECIES, RABBIT_SPECIES, MUSHROOM_SPECIES, CHONK_SPECIES;
static const Species* SPECIES[] = {
  &CAPYBARA_SPECIES, &DUCK_SPECIES, &GOOSE_SPECIES, &BLOB_SPECIES, &CAT_SPECIES,
  &DRAGON_SPECIES, &OCTOPUS_SPECIES, &OWL_SPECIES, &PENGUIN_SPECIES, &TURTLE_SPECIES,
  &SNAIL_SPECIES, &GHOST_SPECIES, &AXOLOTL_SPECIES, &CACTUS_SPECIES, &ROBOT_SPECIES,
  &RABBIT_SPECIES, &MUSHROOM_SPECIES, &CHONK_SPECIES,
};
static const int N_SPECIES = sizeof(SPECIES) / sizeof(SPECIES[0]);
static const char* STATE_NAMES[7] = { "sleep","idle","busy","attention","celebrate","dizzy","heart" };

// ── buttons ──
static const int BTN_H=96, BTN_TOP=H-BTN_H, BTN_GAP=8;
static const int BTN_W=(W-3*BTN_GAP)/2, APPROVE_X=BTN_GAP, DENY_X=BTN_GAP*2+BTN_W;
static void drawButtons(int pressed) {
  gfx.fillRoundRect(APPROVE_X, BTN_TOP, BTN_W, BTN_H, 12, pressed==1?0x07E0:0x02E0);
  gfx.drawRoundRect(APPROVE_X, BTN_TOP, BTN_W, BTN_H, 12, 0x07E0);
  gfx.fillRoundRect(DENY_X, BTN_TOP, BTN_W, BTN_H, 12, pressed==2?0xF800:0x7800);
  gfx.drawRoundRect(DENY_X, BTN_TOP, BTN_W, BTN_H, 12, 0xF800);
  gfx.setTextDatum(MC_DATUM); gfx.setTextSize(3);
  gfx.setTextColor(0xFFFF, pressed==1?0x07E0:0x02E0);
  gfx.drawString("APPROVE", APPROVE_X+BTN_W/2, BTN_TOP+BTN_H/2);
  gfx.setTextColor(0xFFFF, pressed==2?0xF800:0x7800);
  gfx.drawString("DENY", DENY_X+BTN_W/2, BTN_TOP+BTN_H/2);
  gfx.setTextDatum(TL_DATUM);
}

// ── touch ──
static bool touchReadPt(uint16_t& x, uint16_t& y) {
  Wire.beginTransmission(TOUCH_ADDR); Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)TOUCH_ADDR, 5) != 5) return false;
  uint8_t n=Wire.read(), xh=Wire.read(), xl=Wire.read(), yh=Wire.read(), yl=Wire.read();
  if ((n & 0x0F) == 0) return false;
  x=((uint16_t)(xh&0x0F)<<8)|xl; y=((uint16_t)(yh&0x0F)<<8)|yl; return true;
}

static int sp = 4;          // start on the cat (index in SPECIES[])
static uint8_t state = 1;   // idle

void setup() {
  Serial.begin(115200); Serial.setTxTimeoutMs(0); delay(300);
  Serial.printf("\n[stage3c] %d species\n", N_SPECIES);
  dispOk = gfx.begin();
  Wire.begin(IIC_SDA, IIC_SCL, 400000);
  Wire.beginTransmission(TOUCH_ADDR); touchOk = (Wire.endTransmission()==0);
  pinMode(TP_INT, INPUT);
  Serial.printf("[stage3c] disp=%d touch=%d\n", dispOk, touchOk);
}

void loop() {
  static uint32_t nextTick=0, nextState=0; static int flashBtn=0; static uint32_t flashUntil=0;
  static bool touchLatch=false;
  uint32_t now = millis();

  if (now >= nextState) { nextState = now + 3000; state = (state + 1) % 7; }

  uint16_t tx, ty;
  bool down = touchOk && touchReadPt(tx, ty);
  if (down && !touchLatch) {              // edge: act once per touch
    touchLatch = true;
    if (ty >= BTN_TOP) {
      flashBtn = (tx < APPROVE_X + BTN_W) ? 1 : 2;
      flashUntil = now + 250;
      Serial.printf("[stage3c] %s tap\n", flashBtn==1?"APPROVE":"DENY");
    } else {                              // top area: switch species
      sp = (tx < W/2) ? (sp + N_SPECIES - 1) % N_SPECIES : (sp + 1) % N_SPECIES;
      Serial.printf("[stage3c] species -> %s\n", SPECIES[sp]->name);
    }
  } else if (!down) {
    touchLatch = false;
  }
  if (now >= flashUntil) flashBtn = 0;

  if (dispOk && now >= nextTick) {
    nextTick = now + 200;
    static uint32_t tick = 0; tick++;
    gfx.fillSprite(BUDDY_BG);
    StateFn fn = SPECIES[sp]->states[state];
    if (fn) fn(tick);
    gfx.setTextDatum(TC_DATUM); gfx.setTextSize(2); gfx.setTextColor(BUDDY_DIM, BUDDY_BG);
    char hdr[40]; snprintf(hdr, sizeof(hdr), "%s / %s", SPECIES[sp]->name, STATE_NAMES[state]);
    gfx.drawString(hdr, W/2, BTN_TOP - 30);
    gfx.setTextDatum(TL_DATUM);
    drawButtons(flashBtn);
    gfx.pushSprite();
  }
  delay(8);
}
