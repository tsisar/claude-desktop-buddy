#include "../buddy_common.h"

// Shared particle overlays + choreography tables for the species files.
// Each body is a byte-for-byte copy of the canonical block that used to be
// pasted into 13-19 of the src/buddies/*.cpp files — see buddy_common.h.
// Do NOT "improve" the math here: pixel-identity with the old inline
// copies is the contract.

void overlayOrbitStars(uint32_t t, int yOff, uint16_t c1, uint16_t c2) {
  static const int8_t OX[] = { 0, 5, 7, 5, 0, -5, -7, -5 };
  static const int8_t OY[] = { -5, -3, 0, 3, 5, 3, 0, -3 };
  uint8_t p1 = t % 8;
  uint8_t p2 = (t + 4) % 8;
  buddySetColor(c1);
  buddySetCursor(BUDDY_X_CENTER + OX[p1] - 2, BUDDY_Y_OVERLAY + yOff + OY[p1]);
  buddyPrint("*");
  buddySetColor(c2);
  buddySetCursor(BUDDY_X_CENTER + OX[p2] - 2, BUDDY_Y_OVERLAY + yOff + OY[p2]);
  buddyPrint("*");
}

void overlayHearts(uint32_t t) {
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

void overlayConfetti(uint32_t t, const char* a, const char* b) {
  static const uint16_t cols[] = { BUDDY_YEL, BUDDY_HEART, BUDDY_CYAN,
                                   BUDDY_WHITE, BUDDY_GREEN };
  for (int i = 0; i < 6; i++) {
    int phase = (t * 2 + i * 11) % 22;
    int x = BUDDY_X_CENTER - 36 + i * 14;
    int y = BUDDY_Y_OVERLAY - 6 + phase;
    if (y > BUDDY_Y_BASE + 20 || y < 0) continue;
    buddySetColor(cols[i % 5]);
    buddySetCursor(x, y);
    buddyPrint((i + (int)(t / 2)) & 1 ? a : b);
  }
}

void overlayDots(uint32_t t, int x, int y) {
  static const char* const DOTS[] = { ".  ", ".. ", "...", " ..", "  .", "   " };
  buddySetColor(BUDDY_WHITE);
  buddySetCursor(BUDDY_X_CENTER + x, BUDDY_Y_OVERLAY + y);
  buddyPrint(DOTS[t % 6]);
}

void overlayBang(uint32_t t, int x1, int x2, int y2) {
  if ((t / 2) & 1) {
    buddySetColor(BUDDY_YEL);
    buddySetCursor(BUDDY_X_CENTER + x1, BUDDY_Y_OVERLAY);
    buddyPrint("!");
  }
  if ((t / 3) & 1) {
    buddySetColor(BUDDY_YEL);
    buddySetCursor(BUDDY_X_CENTER + x2, BUDDY_Y_OVERLAY + y2);
    buddyPrint("!");
  }
}

const uint8_t BUDDY_DIZZY_SEQ[14]    = { 0,1,0,1, 2,3, 0,1,0,1, 4,4, 2,3 };
const int8_t  BUDDY_DIZZY_XSHIFT[14] = { -3,3,-3,3, 0,0, -3,3,-3,3, 0,0, 0,0 };
const uint8_t BUDDY_CELEB_SEQ[16]    = { 0,1,2,1,0, 3,4,3,4, 0,1,2,1,0, 5,5 };
const int8_t  BUDDY_CELEB_YSHIFT[16] = { 0,-3,-6,-3,0, 0,0,0,0, 0,-3,-6,-3,0, 0,0 };
