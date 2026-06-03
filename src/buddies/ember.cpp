#include "../buddy.h"
#include "../buddy_common.h"

// "ember" — a block-art living flame in Claude terracotta. The body is
// drawn entirely from Unicode Block Elements (U+2580–U+259F) via
// buddyPrintBlocks (see src/blockart.*); a hotter flicker tongue rides
// above it and ASCII particles (z / sparks / ! / hearts / stars) add
// life. All seven persona states are animated.
//
// Authoring grid: 9 cells wide, monospace. Block cells fill seamlessly;
// gaps (spaces) inside the body read as eyes. Motion comes from the
// sprite yOffset (bob/jump) and xOff (lean/wobble); the flame tongue is
// a separate buddyPrintBlocks call stacked two rows above the body.

namespace ember {

static const uint16_t TERRA    = 0xDBAA;  // Claude terracotta (#D97757)
static const uint16_t TERRA_DIM= 0x69C5;  // dimmed terracotta (sleeping)
static const uint16_t EMBER_HOT= 0xFBC6;  // bright flame orange
static const uint16_t SPARK    = BUDDY_YEL;

// Push the body one cell down so it sits clearly below the flame tongue
// instead of merging with it. Applied to every body draw; flame() is left
// at its own offset, so this only widens the gap between them.
static const int DROP = 8;

// ── body poses (3 rows, 9 cells) ───────────────────────────────────────────
static const char* const NEUTRAL[3] = { " ▐▛███▜▌ ", "▝▜█████▛▘", "  ▘▘ ▝▝  " };
static const char* const EYES[3]    = { " ▐▛███▜▌ ", "▝▜█ █ █▛▘", "  ▘▘ ▝▝  " };
static const char* const STRETCH[3] = { "  ▟███▙  ", " ▐█████▌ ", "  ▘▘ ▝▝  " };
static const char* const TALL[3]    = { "  ▟█ █▙  ", " ▐█████▌ ", "  ▘▘ ▝▝  " };
static const char* const SQUASH[3]  = { "         ", "▗▟█████▙▖", " ▘▘   ▝▝ " };
static const char* const FLAT[3]    = { "         ", "▗▟█ █ █▙▖", " ▘▘   ▝▝ " };
static const char* const LEAN_L[3]  = { "▐▛███▜▌  ", "▜█████▛▘ ", " ▘▘ ▝▝   " };
static const char* const LEAN_R[3]  = { "  ▐▛███▜▌", " ▝▜█████▛", "   ▘▘ ▝▝ " };
static const char* const DROWSE[3]  = { "         ", " ▗▄███▄▖ ", "  ▘▘ ▝▝  " };
static const char* const HAPPY[3]   = { " ▐▛███▜▌ ", "▝▜█▘█▝█▛▘", "  ▘▘ ▝▝  " };
// Wink poses: fill one top-row eye-notch (▛ / ▜) so that eye reads closed.
static const char* const WINK_L[3]  = { " ▐████▜▌ ", "▝▜█████▛▘", "  ▘▘ ▝▝  " };
static const char* const WINK_R[3]  = { " ▐▛████▌ ", "▝▜█████▛▘", "  ▘▘ ▝▝  " };

// ── flame tongue frames (2 rows, hot color), centered on col 4 ──────────────
static const char* const FL_L[2] = { "    ▟▖   ", "   ▝█▘   " };
static const char* const FL_R[2] = { "   ▗▙    ", "   ▝█▘   " };
static const char* const FL_HI[2]= { "   ▗▟▖   ", "   ▝█▘   " };
static const char* const FL_LO[2]= { "         ", "    ▝▘   " };

// Draw the flickering flame two rows above the body top (yOffset rows of 8).
static void flame(uint32_t t, int bodyYOff) {
  static const char* const* const F[4] = { FL_L, FL_HI, FL_R, FL_LO };
  buddyPrintBlocks(F[t % 4], 2, bodyYOff - 16, EMBER_HOT);
}

// ─── SLEEP ───  slow breathing ember, drifting "z"s, dimmed
static void doSleep(uint32_t t) {
  const char* const* P[2] = { DROWSE, FLAT };
  static const uint8_t SEQ[] = { 0,0,0,0, 1,1, 0,0,0,0, 0,0 };
  uint8_t beat = (t / 6) % sizeof(SEQ);
  buddyPrintBlocks(P[SEQ[beat]], 3, 2 + DROP, TERRA_DIM);

  // a single low embered glow that pulses
  if ((t / 6) & 1) {
    buddyPrintBlocks(FL_LO, 2, 2 - 16, 0x8800 /*dim red-orange*/);
  }

  int p1 = (t)     % 12;
  int p2 = (t + 5) % 12;
  int p3 = (t + 9) % 12;
  buddySetColor(BUDDY_DIM);
  buddySetCursor(BUDDY_X_CENTER + 18 + p1, BUDDY_Y_OVERLAY + 18 - p1 * 2);
  buddyPrint("z");
  buddySetColor(BUDDY_WHITE);
  buddySetCursor(BUDDY_X_CENTER + 24 + p2, BUDDY_Y_OVERLAY + 14 - p2);
  buddyPrint("Z");
  buddySetColor(BUDDY_DIM);
  buddySetCursor(BUDDY_X_CENTER + 14 + p3 / 2, BUDDY_Y_OVERLAY + 8 - p3 / 2);
  buddyPrint("z");
}

// ─── IDLE ───  steady solid ember: soft bob, occasional look-around, winks.
// No flame tongue and no face gaps here — keeps the clean, recognizable
// Claude ember silhouette while parked (the gapped EYES pose read as an
// open mouth, so idle stays solid). The only face animation is a wink:
// left eye, then later the right.
static void doIdle(uint32_t t) {
  const char* const* P[5] = { NEUTRAL, LEAN_L, LEAN_R, WINK_L, WINK_R };
  static const int8_t SEQ[] = {
    0,0,0,3,0,0,0,0, 0,0, 1,0,2,0, 0,0,0,4,0,0, 0,0
  };
  static const int8_t BOB[]  = {
    0,-1,0,-1,0,-1,0,0, 0,0, 0,0,0,0, 0,-1,0,-1,0,0, 0,-1
  };
  uint8_t beat = (t / 5) % sizeof(SEQ);
  uint8_t pose = SEQ[beat];
  int xOff = (pose == 1) ? -1 : (pose == 2) ? 1 : 0;
  buddyPrintBlocks(P[pose], 3, BOB[beat] + DROP, TERRA, xOff);
}

// ─── BUSY ───  taller focused flame, fast flicker, dot ticker, sparks
static void doBusy(uint32_t t) {
  const char* const* P[3] = { STRETCH, TALL, EYES };
  static const uint8_t SEQ[] = { 0,1,0,1, 2,2, 0,1,0,1, 1,0 };
  uint8_t beat = (t / 3) % sizeof(SEQ);
  buddyPrintBlocks(P[SEQ[beat]], 3, -1 + DROP, TERRA);

  // hot, fast flame
  static const char* const* const FF[2] = { FL_HI, FL_L };
  buddyPrintBlocks(FF[t & 1], 2, -1 - 16, EMBER_HOT);

  static const char* const DOTS[] = { ".  ", ".. ", "...", " ..", "  .", "   " };
  buddySetColor(BUDDY_WHITE);
  buddySetCursor(BUDDY_X_CENTER + 22, BUDDY_Y_OVERLAY + 14);
  buddyPrint(DOTS[t % 6]);

  // a rising spark inside the flame
  int b = (t / 2) % 8;
  buddySetColor(SPARK);
  buddySetCursor(BUDDY_X_CENTER - 1, BUDDY_Y_OVERLAY + 16 - b);
  buddyPrint(b < 6 ? "*" : " ");
}

// ─── ATTENTION ───  bolt upright, wide eyes, "!" pulses, tense shimmy
static void doAttention(uint32_t t) {
  const char* const* P[3] = { TALL, STRETCH, EYES };
  static const uint8_t SEQ[] = { 0,1,0,1,0,1, 2,0, 0,1,0,1, 0,0 };
  uint8_t beat = (t / 4) % sizeof(SEQ);
  int xOff = (t & 1) ? 1 : -1;   // jittery alertness
  buddyPrintBlocks(P[SEQ[beat]], 3, -2 + DROP, EMBER_HOT, xOff);
  flame(t, -2);

  if ((t / 2) & 1) {
    buddySetColor(SPARK);
    buddySetCursor(BUDDY_X_CENTER - 8, BUDDY_Y_OVERLAY - 4);
    buddyPrint("!");
  }
  if ((t / 3) & 1) {
    buddySetColor(BUDDY_RED);
    buddySetCursor(BUDDY_X_CENTER + 8, BUDDY_Y_OVERLAY);
    buddyPrint("!");
  }
  if ((t / 4) & 1) {
    buddySetColor(SPARK);
    buddySetCursor(BUDDY_X_CENTER, BUDDY_Y_OVERLAY - 8);
    buddyPrint("!");
  }
}

// ─── CELEBRATE ───  bouncing flare + confetti/spark rain
static void doCelebrate(uint32_t t) {
  const char* const* P[4] = { SQUASH, STRETCH, TALL, HAPPY };
  static const uint8_t SEQ[]    = { 0,1,2,3,2,1, 0,1,2,3,2,1, 0,0 };
  static const int8_t Y_JUMP[]  = { 1,-2,-7,-9,-7,-2, 1,-2,-7,-9,-7,-2, 1,1 };
  uint8_t beat = (t / 3) % sizeof(SEQ);
  buddyPrintBlocks(P[SEQ[beat]], 3, Y_JUMP[beat] + DROP, EMBER_HOT);
  flame(t, Y_JUMP[beat]);

  static const uint16_t cols[] = { SPARK, BUDDY_HEART, EMBER_HOT, TERRA, BUDDY_RED };
  for (int i = 0; i < 6; i++) {
    int phase = (t * 2 + i * 11) % 22;
    int x = BUDDY_X_CENTER - 36 + i * 14;
    int y = BUDDY_Y_OVERLAY - 6 + phase;
    if (y > BUDDY_Y_BASE + 20 || y < 0) continue;
    buddySetColor(cols[i % 5]);
    buddySetCursor(x, y);
    buddyPrint((i + (int)(t / 2)) & 1 ? "*" : "+");
  }
}

// ─── DIZZY ───  wobbling lean, woozy gap-eyes, orbiting stars
static void doDizzy(uint32_t t) {
  // EYES/FLAT carry the gap "eyes"; leaning + orbiting stars sell the spin.
  const char* const* P[3] = { LEAN_L, LEAN_R, FLAT };
  static const uint8_t SEQ[]   = { 0,1,0,1, 2,2, 0,1,0,1, 2,2 };
  static const int8_t X_SH[]   = { -3,3,-3,3, 0,0, -3,3,-3,3, 0,0 };
  uint8_t beat = (t / 4) % sizeof(SEQ);
  buddyPrintBlocks(P[SEQ[beat]], 3, 1 + DROP, TERRA, X_SH[beat]);

  static const int8_t OX[] = { 0, 5, 7, 5, 0, -5, -7, -5 };
  static const int8_t OY[] = { -5, -3, 0, 3, 5, 3, 0, -3 };
  uint8_t p1 = t % 8, p2 = (t + 4) % 8, p3 = (t + 2) % 8;
  buddySetColor(SPARK);
  buddySetCursor(BUDDY_X_CENTER + OX[p1] - 2, BUDDY_Y_OVERLAY + 4 + OY[p1]);
  buddyPrint("*");
  buddySetColor(EMBER_HOT);
  buddySetCursor(BUDDY_X_CENTER + OX[p2] - 2, BUDDY_Y_OVERLAY + 4 + OY[p2]);
  buddyPrint("*");
  buddySetColor(BUDDY_WHITE);
  buddySetCursor(BUDDY_X_CENTER + OX[p3] - 2, BUDDY_Y_OVERLAY + 4 + OY[p3]);
  buddyPrint("o");
}

// ─── HEART ───  warm content glow, happy eyes, rising hearts
static void doHeart(uint32_t t) {
  const char* const* P[3] = { HAPPY, EYES, NEUTRAL };
  static const uint8_t SEQ[] = { 0,0,1,0, 0,0,1,0, 2,0,1,0, 0,0 };
  static const int8_t BOB[]  = { 0,-1,0,-1, 0,-1,0,-1, 0,0,0,-1, 0,-1 };
  uint8_t beat = (t / 5) % sizeof(SEQ);
  buddyPrintBlocks(P[SEQ[beat]], 3, BOB[beat] + DROP, TERRA);
  flame(t, BOB[beat]);

  buddySetColor(BUDDY_HEART);
  for (int i = 0; i < 5; i++) {
    int phase = (t + i * 4) % 16;
    int y = BUDDY_Y_OVERLAY + 16 - phase;
    if (y < -2 || y > BUDDY_Y_BASE) continue;
    int x = BUDDY_X_CENTER - 20 + i * 8 + ((phase / 3) & 1) * 2 - 1;
    buddySetCursor(x, y);
    buddyPrint("v");
  }
}

}  // namespace ember

extern const Species EMBER_SPECIES = {
  "ember",
  ember::TERRA,
  { ember::doSleep, ember::doIdle, ember::doBusy, ember::doAttention,
    ember::doCelebrate, ember::doDizzy, ember::doHeart }
};