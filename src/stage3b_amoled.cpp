// Stage 3b — animate ONE species (cat, all 7 states) full-screen on AMOLED.
//
// Proves the species animation engine renders through Surface on the big
// screen before wiring up all 18 species and the real state machine. The
// cat's 7 state functions + art are copied verbatim from src/buddies/cat.cpp;
// the buddy_common helpers are reimplemented here backed by Surface instead
// of TFT_eSprite. States auto-cycle every 3s; APPROVE/DENY buttons remain.
//
// Selected by build_src_filter in [env:ws-amoled-18].

#include <Arduino.h>
#include <Wire.h>
#include <string.h>
#include "board_pins.h"
#include "hal/display.h"

// RGB565
static const uint16_t C_BG     = 0x0000;
static const uint16_t C_TEXT   = 0xFFFF;
static const uint16_t C_DIM    = 0x8410;
static const uint16_t C_GREEN  = 0x07E0;
static const uint16_t C_RED    = 0xF800;
static const uint16_t C_DGREEN = 0x02E0;
static const uint16_t C_DRED   = 0x7800;
static const uint16_t CAT_BODY = 0xC2A6;

// buddy_common palette names used by the cat art
static const uint16_t BUDDY_BG    = 0x0000;
static const uint16_t BUDDY_HEART = 0xF810;
static const uint16_t BUDDY_DIM   = 0x8410;
static const uint16_t BUDDY_YEL   = 0xFFE0;
static const uint16_t BUDDY_WHITE = 0xFFFF;
static const uint16_t BUDDY_CYAN  = 0x07FF;
static const uint16_t BUDDY_GREEN = 0x07E0;

static Surface gfx;
static bool dispOk = false, touchOk = false;

// ── geometry (scaled up for 368x448) ──
static const int W = LCD_WIDTH, H = LCD_HEIGHT;
static const int BUDDY_X_CENTER = W / 2;     // 184
static const int BUDDY_Y_BASE   = 70;        // top of body block (px)
static const int BUDDY_Y_OVERLAY= 30;        // particle band above body (px)
static const int BUDDY_CHAR_W   = 6;
static const int BUDDY_CHAR_H   = 8;
static const uint8_t SCALE = 4;              // glyph 24x32 at size 4

// ── Surface-backed buddy_common helpers (mirror src/buddy.cpp) ──
static void buddyPrintLine(const char* line, int yPx, uint16_t color, int xOff = 0) {
  int len = strlen(line);
  while (len && line[len-1] == ' ') len--;          // trim for centering at scale
  while (len && *line == ' ') { line++; len--; }
  int w = len * BUDDY_CHAR_W * SCALE;
  int x = BUDDY_X_CENTER - w / 2 + xOff * SCALE;
  gfx.setTextColor(color, BUDDY_BG);
  gfx.setCursor(x, yPx);
  for (int i = 0; i < len; i++) gfx.print(line[i]);
}
static void buddyPrintSprite(const char* const* lines, uint8_t n, int yOffset,
                             uint16_t color, int xOff = 0) {
  gfx.setTextSize(SCALE);
  for (uint8_t i = 0; i < n; i++)
    buddyPrintLine(lines[i], BUDDY_Y_BASE + (yOffset + i * BUDDY_CHAR_H) * SCALE, color, xOff);
}
static void buddySetCursor(int x, int y) {
  gfx.setCursor(BUDDY_X_CENTER + (x - BUDDY_X_CENTER) * SCALE, y * SCALE);
}
static void buddySetColor(uint16_t fg) { gfx.setTextColor(fg, BUDDY_BG); }
static void buddyPrint(const char* s)  { gfx.setTextSize(SCALE); gfx.print(s); }

// ════════════════ cat species (verbatim art from buddies/cat.cpp) ════════════════
namespace cat {
static void doSleep(uint32_t t) {
  static const char* const LOAF[5]    = { "            ", "            ", "   .-..-.   ", "  ( -.- )   ", "  `------`~ " };
  static const char* const BREATHE[5] = { "            ", "            ", "   .-..-.   ", "  ( -.- )_  ", " `~------'~ " };
  static const char* const CURL[5]    = { "            ", "            ", "   .-/\\.    ", "  (  ..  )) ", "  `~~~~~~`  " };
  static const char* const CURL_TW[5] = { "            ", "            ", "   .-/\\.    ", "  (  ..  )) ", "  `~~~~~~`~ " };
  static const char* const PURR[5]    = { "            ", "            ", "   .-..-.   ", "  ( u.u )   ", " `~------'~ " };
  static const char* const DREAM[5]   = { "            ", "            ", "   .-..-.   ", "  ( o.o )   ", "  `------`  " };
  const char* const* P[6] = { LOAF, BREATHE, LOAF, PURR, CURL, CURL_TW };
  static const uint8_t SEQ[] = { 0,1,0,1,0,1, 3,3,0,1, 4,5,4,5,4,5, 2,2, 0,1,0,1, 5,5,4,4 };
  buddyPrintSprite(P[SEQ[(t/5)%sizeof(SEQ)]], 5, 0, CAT_BODY);
  int p1=(t)%12, p2=(t+5)%12, p3=(t+9)%12;
  buddySetColor(BUDDY_DIM);   buddySetCursor(BUDDY_X_CENTER+18+p1, BUDDY_Y_OVERLAY+18-p1*2); buddyPrint("z");
  buddySetColor(BUDDY_WHITE); buddySetCursor(BUDDY_X_CENTER+24+p2, BUDDY_Y_OVERLAY+14-p2);   buddyPrint("Z");
  buddySetColor(BUDDY_DIM);   buddySetCursor(BUDDY_X_CENTER+14+p3/2, BUDDY_Y_OVERLAY+8-p3/2); buddyPrint("z");
}
static void doIdle(uint32_t t) {
  static const char* const REST[5]   = { "            ", "   /\\_/\\    ", "  ( o   o ) ", "  (  w   )  ", "  (\")_(\")   " };
  static const char* const LOOK_L[5] = { "            ", "   /\\_/\\    ", "  (o    o ) ", "  (  w   )  ", "  (\")_(\")   " };
  static const char* const LOOK_R[5] = { "            ", "   /\\_/\\    ", "  ( o    o) ", "  (  w   )  ", "  (\")_(\")   " };
  static const char* const BLINK[5]  = { "            ", "   /\\_/\\    ", "  ( -   - ) ", "  (  w   )  ", "  (\")_(\")   " };
  static const char* const SLOW_BL[5]= { "            ", "   /\\-/\\    ", "  ( _   _ ) ", "  (  w   )  ", "  (\")_(\")   " };
  static const char* const EAR_L[5]  = { "            ", "   <\\_/\\    ", "  ( o   o ) ", "  (  w   )  ", "  (\")_(\")   " };
  static const char* const EAR_R[5]  = { "            ", "   /\\_/>    ", "  ( o   o ) ", "  (  w   )  ", "  (\")_(\")   " };
  static const char* const TAIL_L[5] = { "            ", "   /\\_/\\    ", "  ( o   o ) ", "  (  w   )  ", "  (\")_(\")~  " };
  static const char* const TAIL_R[5] = { "            ", "   /\\_/\\    ", "  ( o   o ) ", "  (  w   )  ", " ~(\")_(\")   " };
  static const char* const GROOM[5]  = { "            ", "   /\\_/\\    ", "  ( ^   ^ ) ", "  (  P   )  ", "  (\")_(\")   " };
  const char* const* P[10] = { REST, LOOK_L, LOOK_R, BLINK, SLOW_BL, EAR_L, EAR_R, TAIL_L, TAIL_R, GROOM };
  static const uint8_t SEQ[] = { 0,0,0,3,0,1,0,2,0, 7,8,7,8,7, 0,5,0,6,0, 4,4,0, 9,9,9,0, 0,3,0, 8,7,8,7, 0,0,4,0 };
  buddyPrintSprite(P[SEQ[(t/5)%sizeof(SEQ)]], 5, 0, CAT_BODY);
}
static void doBusy(uint32_t t) {
  static const char* const PAW_UP[5] = { "      .     ", "   /\\_/\\    ", "  ( o   o ) ", "  (  w   )/ ", "  (\")_(\")   " };
  static const char* const PAW_TAP[5]= { "    .       ", "   /\\_/\\    ", "  ( o   o ) ", "  (  w   )_ ", "  (\")_(\")   " };
  static const char* const STARE[5]  = { "            ", "   /\\_/\\    ", "  ( O   O ) ", "  (  w   )  ", "  (\")_(\")   " };
  static const char* const NUDGE[5]  = { "    o       ", "   /\\_/\\    ", "  ( o   o ) ", "  ( -w   )  ", "  (\")_(\")   " };
  static const char* const SHOVE[5]  = { "  o         ", "   /\\_/\\    ", "  ( o   o ) ", "  (-w    )  ", "  (\")_(\")   " };
  static const char* const SMUG[5]   = { "            ", "   /\\_/\\    ", "  ( -   - ) ", "  (  w   )  ", "  (\")_(\")   " };
  const char* const* P[6] = { PAW_UP, PAW_TAP, STARE, NUDGE, SHOVE, SMUG };
  static const uint8_t SEQ[] = { 2,2,2, 0,1,0,1, 3,4,3,4, 5,5, 2,2, 0,1,0,1, 5,2 };
  buddyPrintSprite(P[SEQ[(t/5)%sizeof(SEQ)]], 5, 0, CAT_BODY);
  static const char* const DOTS[] = { ".  ", ".. ", "...", " ..", "  .", "   " };
  buddySetColor(BUDDY_WHITE); buddySetCursor(BUDDY_X_CENTER+22, BUDDY_Y_OVERLAY+14); buddyPrint(DOTS[t%6]);
}
static void doAttention(uint32_t t) {
  static const char* const ALERT[5]  = { "            ", "   /^_^\\    ", "  ( O   O ) ", "  (  v   )  ", "  (\")_(\")   " };
  static const char* const SCAN_L[5] = { "            ", "   /^_^\\    ", "  (O    O ) ", "  (  v   )  ", "  (\")_(\")   " };
  static const char* const SCAN_R[5] = { "            ", "   /^_^\\    ", "  ( O    O) ", "  (  v   )  ", "  (\")_(\")   " };
  static const char* const SCAN_U[5] = { "            ", "   /^_^\\    ", "  ( ^   ^ ) ", "  (  v   )  ", "  (\")_(\")   " };
  static const char* const CROUCH[5] = { "            ", "   /^_^\\    ", " /( O   O )\\", " (   v    ) ", " /(\")_(\")\\  " };
  static const char* const HISS[5]   = { "            ", "   /^_^\\    ", "  ( O   O ) ", "  (  >   )  ", "  (\")_(\")   " };
  const char* const* P[6] = { ALERT, SCAN_L, SCAN_R, SCAN_U, CROUCH, HISS };
  static const uint8_t SEQ[] = { 0,4,0,1,0,2,0,3, 4,4,0,1,2,0, 5,0 };
  uint8_t pose = SEQ[(t/5)%sizeof(SEQ)];
  int xOff = (pose == 4) ? ((t & 1) ? 1 : -1) : 0;
  buddyPrintSprite(P[pose], 5, 0, CAT_BODY, xOff);
  if ((t/2)&1) { buddySetColor(BUDDY_YEL); buddySetCursor(BUDDY_X_CENTER-4, BUDDY_Y_OVERLAY);   buddyPrint("!"); }
  if ((t/3)&1) { buddySetColor(BUDDY_YEL); buddySetCursor(BUDDY_X_CENTER+4, BUDDY_Y_OVERLAY+4); buddyPrint("!"); }
}
static void doCelebrate(uint32_t t) {
  static const char* const CROUCH[5] = { "            ", "   /\\_/\\    ", "  ( ^   ^ ) ", "  (  W   )  ", " /(\")_(\")\\  " };
  static const char* const JUMP[5]   = { "  \\^   ^/   ", "    /\\_/\\   ", "  ( ^   ^ ) ", "  (  W   )  ", "  (\")_(\")   " };
  static const char* const PEAK[5]   = { "  \\^   ^/   ", "    /\\_/\\   ", "  ( * * * ) ", "  (  W   )  ", "  (\")_(\")~  " };
  static const char* const SPIN_L[5] = { "            ", "   /\\_/\\    ", "  ( <   < ) ", "  (  W   ) /", " ~(\")_(\")   " };
  static const char* const SPIN_R[5] = { "            ", "   /\\_/\\    ", "  ( >   > ) ", " \\(  W   )  ", "  (\")_(\")~  " };
  static const char* const POSE[5]   = { "    \\o/     ", "   /\\_/\\    ", "  ( ^   ^ ) ", " /(  W   )\\ ", "  (\")_(\")   " };
  const char* const* P[6] = { CROUCH, JUMP, PEAK, SPIN_L, SPIN_R, POSE };
  static const uint8_t SEQ[] = { 0,1,2,1,0, 3,4,3,4, 0,1,2,1,0, 5,5 };
  static const int8_t Y_SHIFT[] = { 0,-3,-6,-3,0, 0,0,0,0, 0,-3,-6,-3,0, 0,0 };
  uint8_t beat = (t/3)%sizeof(SEQ);
  buddyPrintSprite(P[SEQ[beat]], 5, Y_SHIFT[beat], CAT_BODY);
  static const uint16_t cols[] = { BUDDY_YEL, BUDDY_HEART, BUDDY_CYAN, BUDDY_WHITE, BUDDY_GREEN };
  for (int i = 0; i < 6; i++) {
    int phase = (t*2 + i*11) % 22;
    int x = BUDDY_X_CENTER - 36 + i*14;
    int y = BUDDY_Y_OVERLAY - 6 + phase;
    if (y > BUDDY_Y_BASE/SCALE + 20 || y < 0) continue;
    buddySetColor(cols[i%5]); buddySetCursor(x, y); buddyPrint((i+(int)(t/2))&1 ? "*" : ".");
  }
}
static void doDizzy(uint32_t t) {
  static const char* const TILT_L[5] = { "            ", "  /\\_/\\     ", " ( @   @ )  ", " (   ~~  )  ", " (\")_(\")    " };
  static const char* const TILT_R[5] = { "            ", "    /\\_/\\   ", "  ( @   @ ) ", "  (  ~~  )  ", "    (\")_(\") " };
  static const char* const WOOZY[5]  = { "            ", "   /\\_/\\    ", "  ( x   @ ) ", "  (  v   )  ", "  (\")_(\")~  " };
  static const char* const WOOZY2[5] = { "            ", "   /\\_/\\    ", "  ( @   x ) ", "  (  v   )  ", " ~(\")_(\")   " };
  static const char* const SPLAT[5]  = { "            ", "   /\\_/\\    ", "  ( @   @ ) ", "  (  -   )  ", " /(\")_(\")\\~ " };
  const char* const* P[5] = { TILT_L, TILT_R, WOOZY, WOOZY2, SPLAT };
  static const uint8_t SEQ[] = { 0,1,0,1, 2,3, 0,1,0,1, 4,4, 2,3 };
  static const int8_t X_SHIFT[] = { -3,3,-3,3, 0,0, -3,3,-3,3, 0,0, 0,0 };
  uint8_t beat = (t/4)%sizeof(SEQ);
  buddyPrintSprite(P[SEQ[beat]], 5, 0, CAT_BODY, X_SHIFT[beat]);
  static const int8_t OX[] = { 0,5,7,5,0,-5,-7,-5 }, OY[] = { -5,-3,0,3,5,3,0,-3 };
  uint8_t p1=t%8, p2=(t+4)%8;
  buddySetColor(BUDDY_CYAN); buddySetCursor(BUDDY_X_CENTER+OX[p1]-2, BUDDY_Y_OVERLAY+6+OY[p1]); buddyPrint("*");
  buddySetColor(BUDDY_YEL);  buddySetCursor(BUDDY_X_CENTER+OX[p2]-2, BUDDY_Y_OVERLAY+6+OY[p2]); buddyPrint("*");
}
static void doHeart(uint32_t t) {
  static const char* const DREAMY[5] = { "            ", "   /\\_/\\    ", "  ( ^   ^ ) ", "  (  u   )  ", "  (\")_(\")~  " };
  static const char* const BLUSH[5]  = { "            ", "   /\\_/\\    ", "  (#^   ^#) ", "  (  u   )  ", "  (\")_(\")   " };
  static const char* const HEART_E[5]= { "            ", "   /\\_/\\    ", "  ( <3 <3 ) ", "  (  u   )  ", "  (\")_(\")~  " };
  static const char* const PURR[5]   = { "            ", "   /\\-/\\    ", "  ( ~   ~ ) ", "  (  u   )  ", " ~(\")_(\")~  " };
  static const char* const HEAD_T[5] = { "            ", "   /\\_/\\    ", "  ( ^   - ) ", "  (  u   )  ", "  (\")_(\")   " };
  const char* const* P[5] = { DREAMY, BLUSH, HEART_E, PURR, HEAD_T };
  static const uint8_t SEQ[] = { 0,0,1,0, 2,2,0, 1,0,4, 0,0,3,3, 0,1,0,2, 1,0 };
  static const int8_t Y_BOB[] = { 0,-1,0,-1, 0,-1,0, -1,0,0, -1,0,0,0, -1,0,-1,0, -1,0 };
  uint8_t beat = (t/5)%sizeof(SEQ);
  buddyPrintSprite(P[SEQ[beat]], 5, Y_BOB[beat], CAT_BODY);
  buddySetColor(BUDDY_HEART);
  for (int i = 0; i < 5; i++) {
    int phase = (t + i*4) % 16;
    int y = BUDDY_Y_OVERLAY + 16 - phase;
    if (y < -2 || y > BUDDY_Y_BASE/SCALE) continue;
    int x = BUDDY_X_CENTER - 20 + i*8 + ((phase/3)&1)*2 - 1;
    buddySetCursor(x, y); buddyPrint("v");
  }
}
}  // namespace cat

typedef void (*StateFn)(uint32_t);
static StateFn STATES[7] = { cat::doSleep, cat::doIdle, cat::doBusy, cat::doAttention,
                             cat::doCelebrate, cat::doDizzy, cat::doHeart };
static const char* STATE_NAMES[7] = { "sleep","idle","busy","attention","celebrate","dizzy","heart" };

// ── buttons (same as stage3a) ──
static const int BTN_H=96, BTN_TOP=H-BTN_H, BTN_GAP=8;
static const int BTN_W=(W-3*BTN_GAP)/2, APPROVE_X=BTN_GAP, DENY_X=BTN_GAP*2+BTN_W;
static void drawButtons(int pressed) {
  gfx.fillRoundRect(APPROVE_X, BTN_TOP, BTN_W, BTN_H, 12, pressed==1?C_GREEN:C_DGREEN);
  gfx.drawRoundRect(APPROVE_X, BTN_TOP, BTN_W, BTN_H, 12, C_GREEN);
  gfx.fillRoundRect(DENY_X, BTN_TOP, BTN_W, BTN_H, 12, pressed==2?C_RED:C_DRED);
  gfx.drawRoundRect(DENY_X, BTN_TOP, BTN_W, BTN_H, 12, C_RED);
  gfx.setTextDatum(MC_DATUM); gfx.setTextSize(3);
  gfx.setTextColor(C_TEXT, pressed==1?C_GREEN:C_DGREEN);
  gfx.drawString("APPROVE", APPROVE_X+BTN_W/2, BTN_TOP+BTN_H/2);
  gfx.setTextColor(C_TEXT, pressed==2?C_RED:C_DRED);
  gfx.drawString("DENY", DENY_X+BTN_W/2, BTN_TOP+BTN_H/2);
  gfx.setTextDatum(TL_DATUM);
}

// ── touch ──
static bool ft3168Read(uint16_t& x, uint16_t& y) {
  Wire.beginTransmission(TOUCH_ADDR); Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)TOUCH_ADDR, 5) != 5) return false;
  uint8_t n=Wire.read(), xh=Wire.read(), xl=Wire.read(), yh=Wire.read(), yl=Wire.read();
  if ((n & 0x0F) == 0) return false;
  x=((uint16_t)(xh&0x0F)<<8)|xl; y=((uint16_t)(yh&0x0F)<<8)|yl; return true;
}

static uint8_t curState = 1;   // start idle
static uint32_t tick = 0;

void setup() {
  Serial.begin(115200); Serial.setTxTimeoutMs(0); delay(300);
  Serial.println("\n[stage3b] animate cat, 7 states");
  dispOk = gfx.begin();
  Wire.begin(IIC_SDA, IIC_SCL, 400000);
  Wire.beginTransmission(TOUCH_ADDR); touchOk = (Wire.endTransmission()==0);
  pinMode(TP_INT, INPUT);
  Serial.printf("[stage3b] disp=%d touch=%d\n", dispOk, touchOk);
}

void loop() {
  static uint32_t nextTick = 0, nextState = 0;
  static int flashBtn = 0; static uint32_t flashUntil = 0;
  uint32_t now = millis();

  // auto-cycle state every 3s
  if (now >= nextState) { nextState = now + 3000; curState = (curState + 1) % 7;
    Serial.printf("[stage3b] state -> %s\n", STATE_NAMES[curState]); }

  // touch → flash a button
  uint16_t tx, ty;
  if (touchOk && ft3168Read(tx, ty) && ty >= BTN_TOP) {
    int b = (tx < APPROVE_X+BTN_W) ? 1 : 2;
    if (b != flashBtn) Serial.printf("[stage3b] %s tap\n", b==1?"APPROVE":"DENY");
    flashBtn = b; flashUntil = now + 250;
  }
  if (now >= flashUntil) flashBtn = 0;

  // render at ~5fps
  if (dispOk && now >= nextTick) {
    nextTick = now + 200; tick++;
    gfx.fillSprite(C_BG);
    STATES[curState](tick);
    // state label
    gfx.setTextDatum(TC_DATUM); gfx.setTextSize(2); gfx.setTextColor(C_DIM, C_BG);
    gfx.drawString(STATE_NAMES[curState], W/2, BTN_TOP - 30);
    gfx.setTextDatum(TL_DATUM);
    drawButtons(flashBtn);
    gfx.pushSprite();
  }
  delay(8);
}
