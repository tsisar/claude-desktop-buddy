#include "../buddy.h"
#include "../buddy_common.h"

// "ember" — a block-art living flame in Claude terracotta. The body is
// drawn entirely from Unicode Block Elements (U+2580–U+259F) via
// buddyPrintBlocks (see src/blockart.*); a hotter flicker tongue is reserved
// for BUSY, and ASCII particles (z / sparks / ! / hearts / stars) add life.
// All seven persona states are animated; each state's header comment shows
// the composed scene the way it reads on screen.
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
static const char* const FLAT[3]    = { "         ", "▗▟█ █ █▙▖", " ▘▘   ▝▝ " };
static const char* const LEAN_L[3]  = { "▐▛███▜▌  ", "▜█████▛▘ ", " ▘▘ ▝▝   " };
static const char* const LEAN_R[3]  = { "  ▐▛███▜▌", " ▝▜█████▛", "   ▘▘ ▝▝ " };
static const char* const DROWSE[3]  = { "         ", " ▗▄███▄▖ ", "  ▘▘ ▝▝  " };
// Wink poses: fill one top-row eye-notch (▛ / ▜) so that eye reads closed.
static const char* const WINK_L[3]  = { " ▐████▜▌ ", "▝▜█████▛▘", "  ▘▘ ▝▝  " };
static const char* const WINK_R[3]  = { " ▐▛████▌ ", "▝▜█████▛▘", "  ▘▘ ▝▝  " };
// ATTENTION poses. ALERT throws both hands up (▄ beside the head) — it
// lives on a 10-cell grid, so doAttention re-centers it with xOff -3.
// LOOK_L/LOOK_R slide the eye-notches across the face (eyes darting).
static const char* const ALERT[3]   = { " ▄▐▛███▜▌▄", " ▝▜█████▛▘", "   ▘▘ ▝▝  " };
static const char* const LOOK_L[3]  = { " ▐▜██▛█▌ ", "▝▜█████▛▘", "  ▘▘ ▝▝  " };
static const char* const LOOK_R[3]  = { " ▐█▜██▛▌ ", "▝▜█████▛▘", "  ▘▘ ▝▝  " };
// CELEBRATE dance poses: one hand up high (▄ beside the head), the other
// arm swung down (▙ / ▟ off the body's far corner). Mirrored pair on the
// same 10-cell grid as ALERT (re-centered with xOff -3).
static const char* const DANCE_L[3] = { " ▄▐▛███▜▌ ", " ▝▜█████▛▙", "   ▘▘ ▝▝  " };
static const char* const DANCE_R[3] = { "  ▐▛███▜▌▄", " ▟▜█████▛▘", "   ▘▘ ▝▝  " };

// ── flame tongue frames (2 rows, hot color), centered on col 4 ──────────────
static const char* const FL_L[2] = { "    ▟▖   ", "   ▝█▘   " };
static const char* const FL_R[2] = { "   ▗▙    ", "   ▝█▘   " };
static const char* const FL_HI[2]= { "   ▗▟▖   ", "   ▝█▘   " };
static const char* const FL_LO[2]= { "         ", "    ▝▘   " };

// ── BUSY laptop-scene rows ──────────────────────────────────────────────────
// Unlike the 3-row poses above, these are drawn one row at a time with a
// per-row xOff (see doBusy for why). Comments give cell width W and start
// column k on the scene grid; xOff = 6k - 45 + 3W.
static const char* const BUSY_HEAD      = "▐█▜██▛▌";    // W7 k2  → -12
static const char* const BUSY_HEAD_HAND = "▐█▜██▛▌▂";   // W8 k2  → -9
static const char* const BUSY_ARM_DOWN  = "▐█████▛▙";   // W8 k2  → -9
static const char* const BUSY_ARM_MID   = "▐█████▛▀";   // W8 k2  → -9
static const char* const BUSY_FEET      = "▘▘ ▝▝";      // W5 k3  → -12
static const char* const LAPTOP_LID     = "▄";          // W1 k13 → 36
static const char* const LAPTOP_SCREEN  = "▟▘";         // W2 k12 → 33
static const char* const LAPTOP_DECK    = "▀▀▀▘";       // W4 k9  → 21

// ── HEART hug (2 rows, pink, drawn IN FRONT of the body) ───────────────────
// Drawn as a second layer over the chest. blockart clears each cell to bg
// before filling, so the black margins around the lobes read as the
// heart's outline against the terracotta. A half-cell xOff (+3) aligns
// the even-width heart onto the odd-width body grid.
static const char* const HUG_HEART[2] = { "▗▖▗▖", "▜▛" };

// Draw the flickering flame two rows above the body top (yOffset rows of 8).
// xOff follows the body when a pose sits off-center (the BUSY laptop scene).
static void flame(uint32_t t, int bodyYOff, int xOff = 0) {
  static const char* const* const F[4] = { FL_L, FL_HI, FL_R, FL_LO };
  buddyPrintBlocks(F[t % 4], 2, bodyYOff - 16, EMBER_HOT, xOff);
}

// ─── SLEEP ───  dimmed ember breathing low, z's drifting up and right:
//
//                  z
//      ▗▄███▄▖   Z          DROWSE for four beats, then a two-beat FLAT
//       ▘▘ ▝▝   z           "exhale" (▗▟█ █ █▙▖ — the gaps read as
//                           half-open eyes), then settle back down.
//
// Three z particles ride different 12-tick phases so the trail never
// repeats in lockstep; the middle Z is white, the others dim gray.
static void doSleep(uint32_t t) {
  const char* const* P[2] = { DROWSE, FLAT };
  static const uint8_t SEQ[] = { 0,0,0,0, 1,1, 0,0,0,0, 0,0 };
  uint8_t beat = (t / 6) % sizeof(SEQ);
  buddyPrintBlocks(P[SEQ[beat]], 3, 2 + DROP, TERRA_DIM);

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

// ─── IDLE ───  steady solid ember: soft bob, look-around leans, winks.
//
//      ▐▛███▜▌              NEUTRAL with a gentle one-px bob; now and
//     ▝▜█████▛▘             then a LEAN_L / LEAN_R glance (±1 shove) or
//       ▘▘ ▝▝               a one-beat wink — ▐████▜▌ / ▐▛████▌, the
//                           eye-notch fills so that eye reads closed.
//
// No flame tongue and no face gaps here — keeps the clean, recognizable
// Claude ember silhouette while parked (the gapped EYES pose read as an
// open mouth, so idle stays solid).
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

// ─── BUSY ───  hammering away at a gray laptop. The whole 14-cell scene:
//
//     ▐█▜██▛▌▂   ▄           body (terracotta)  +  laptop (gray)
//     ▐█████▛▙  ▟▘           the typing hand bobs: wound up high (▂ above
//      ▘▘ ▝▝ ▀▀▀▘            the arm) ↔ dropped onto the keys (▛▀)
//
// Rows are drawn one at a time with a per-row xOff: block centering is
// per line, and padding the strings to one width can't work here — a
// space paints bg, so the body's padding would erase the laptop (and
// vice versa). Offsets are derived from the 15-cell grid: a row of W
// cells starting at cell k needs xOff = 6k - 45 + 3W.
static void doBusy(uint32_t t) {
  // Two poses — hand wound up high (▂ floats over the arm) vs dropped onto
  // the keys (▛▀). Irregular rhythm reads like real typing, not a metronome.
  static const uint8_t KEYS[] = { 0,1,0,1,1,0, 0,1,0,1,0,0, 1,0,1,1 };
  bool up = KEYS[t % sizeof(KEYS)];

  const int yb = -1 + DROP;
  const char* one[1];
  one[0] = up ? BUSY_HEAD_HAND : BUSY_HEAD;
  buddyPrintBlocks(one, 1, yb,      TERRA, up ? -9 : -12);
  one[0] = up ? BUSY_ARM_DOWN : BUSY_ARM_MID;
  buddyPrintBlocks(one, 1, yb + 8,  TERRA, -9);
  one[0] = BUSY_FEET;
  buddyPrintBlocks(one, 1, yb + 16, TERRA, -12);

  // Laptop on the far side of the deck, gray.
  one[0] = LAPTOP_LID;    buddyPrintBlocks(one, 1, yb,      BUDDY_DIM, 36);
  one[0] = LAPTOP_SCREEN; buddyPrintBlocks(one, 1, yb + 8,  BUDDY_DIM, 33);
  one[0] = LAPTOP_DECK;   buddyPrintBlocks(one, 1, yb + 16, BUDDY_DIM, 21);

  // The flickering flame tongue (L → HI → R → LO), tracking the body.
  flame(t, -1, -12);

  // Output ticking across the laptop screen.
  overlayDots(t, 24, 12);
}

// ─── ATTENTION ───  startled: hands up, eyes darting:
//
//      !            !       ALERT holds the base — both hands thrown up
//     ▄▐▛███▜▌▄  !          beside the head; between pulses LOOK_L /
//     ▝▜█████▛▘             LOOK_R slide the eye-notches across the
//       ▘▘ ▝▝               face. ±1 px jitter the whole time; three
//                           "!" particles blink on co-prime periods
//                           (2/3/4 ticks) in spark-yellow / red.
static void doAttention(uint32_t t) {
  const char* const* P[3] = { ALERT, LOOK_L, LOOK_R };
  static const uint8_t SEQ[] = { 0,0,1,0, 0,2,0,0, 1,2,0,0 };
  uint8_t beat = (t / 4) % sizeof(SEQ);
  uint8_t pose = SEQ[beat];
  // ALERT sits on a 10-cell grid — half a cell (-3) re-centers its body
  // onto the 9-cell grid the look poses use, so the body doesn't hop.
  int xOff = (pose == 0 ? -3 : 0) + ((t & 1) ? 1 : -1);   // jittery alertness
  buddyPrintBlocks(P[pose], 3, -2 + DROP, TERRA, xOff);

  // 5 logical px (20 screen px) below the classic overlay row — the top
  // strip is where the approval "approve? Ns" banner rides, and the "!"
  // marks were poking into it.
  const int yq = BUDDY_Y_OVERLAY + 5;
  if ((t / 2) & 1) {
    buddySetColor(SPARK);
    buddySetCursor(BUDDY_X_CENTER - 8, yq - 4);
    buddyPrint("!");
  }
  if ((t / 3) & 1) {
    buddySetColor(BUDDY_RED);
    buddySetCursor(BUDDY_X_CENTER + 8, yq);
    buddyPrint("!");
  }
  if ((t / 4) & 1) {
    buddySetColor(SPARK);
    buddySetCursor(BUDDY_X_CENTER, yq - 8);
    buddyPrint("!");
  }
}

// ─── CELEBRATE ───  dancing — arms swing in counterpoint — while bouncing:
//
//    *  ▄▐▛███▜▌   +        DANCE_L ↔ DANCE_R alternate every beat (left
//     + ▝▜█████▛▙ *         hand up / right arm down, then mirrored)
//    *    ▘▘ ▝▝   +         riding the jump arc to -9 px and back. Six
//                           confetti streams (*/+) fall on staggered
//                           22-tick phases in five colors.
static void doCelebrate(uint32_t t) {
  const char* const* P[2] = { DANCE_L, DANCE_R };
  static const int8_t Y_JUMP[]  = { 1,-2,-7,-9,-7,-2, 1,-2,-7,-9,-7,-2, 1,1 };
  uint8_t beat = (t / 3) % sizeof(Y_JUMP);
  // -3: the dance poses live on a 10-cell grid (see the ATTENTION note).
  buddyPrintBlocks(P[(t / 3) & 1], 3, Y_JUMP[beat] + DROP, TERRA, -3);

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

// ─── DIZZY ───  wobbling lean, woozy gap-eyes, orbiting stars:
//
//        * o                LEAN_L ↔ LEAN_R shoved ±3 px each beat, then
//     ▜█████▛▘ *            a two-beat FLAT collapse (▗▟█ █ █▙▖, the
//      ▘▘ ▝▝                gaps read as woozy eyes). Three particles
//                           (* * o) orbit an 8-point ellipse around the
//                           face, offset from each other by thirds.
static void doDizzy(uint32_t t) {
  // FLAT carries the gap "eyes"; leaning + orbiting stars sell the spin.
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

// ─── HEART ───  hugging a pink heart to its chest:
//
//      v  v  v
//      ▐▛███▜▌              NEUTRAL body with a soft bob; the pink
//     ▝▜█▗▖▗▖█▛▘            HUG_HEART (▗▖▗▖ / ▜▛) sits in front of the
//       ▘▘▜▛▝▝              chest as a second layer, bobbing with the
//                           body. Five "v" hearts still rise past the
//                           face on staggered 16-tick phases.
static void doHeart(uint32_t t) {
  static const int8_t BOB[] = { 0,-1,0,-1, 0,-1,0,-1, 0,0,0,-1, 0,-1 };
  uint8_t beat = (t / 5) % sizeof(BOB);
  int y = BOB[beat] + DROP;
  buddyPrintBlocks(NEUTRAL, 3, y, TERRA);
  buddyPrintBlocks(HUG_HEART, 2, y + 8, BUDDY_HEART, 3);   // front layer

  overlayHearts(t);
}

}  // namespace ember

extern const Species EMBER_SPECIES = {
  "ember",
  ember::TERRA,
  { ember::doSleep, ember::doIdle, ember::doBusy, ember::doAttention,
    ember::doCelebrate, ember::doDizzy, ember::doHeart }
};
