#pragma once
#include <stdint.h>

// Shared constants and helpers for buddy species files.
// Each species file (src/buddies/<name>.cpp) includes this header
// and defines its 7 state functions.

// Geometry — shared layout for all species
extern const int BUDDY_X_CENTER;
extern const int BUDDY_CANVAS_W;
extern const int BUDDY_Y_BASE;
extern const int BUDDY_Y_OVERLAY;
extern const int BUDDY_CHAR_W;
extern const int BUDDY_CHAR_H;

// Common colors species can use freely
extern const uint16_t BUDDY_BG;
extern const uint16_t BUDDY_HEART;
extern const uint16_t BUDDY_DIM;
extern const uint16_t BUDDY_YEL;
extern const uint16_t BUDDY_WHITE;
extern const uint16_t BUDDY_CYAN;
extern const uint16_t BUDDY_GREEN;
extern const uint16_t BUDDY_PURPLE;
extern const uint16_t BUDDY_RED;
extern const uint16_t BUDDY_BLUE;

// Print one line centered around BUDDY_X_CENTER, optionally x-offset.
void buddyPrintLine(const char* line, int yPx, uint16_t color, int xOff = 0);

// Print N-line sprite block. yOffset is added to BUDDY_Y_BASE for the top row.
void buddyPrintSprite(const char* const* lines, uint8_t nLines, int yOffset, uint16_t color, int xOff = 0);

// Like buddyPrintSprite, but Unicode Block Elements (U+2580–U+259F) in the
// art are filled as seamless rectangles instead of font glyphs (▐ ▛ █ ▜ ▌
// ▘ ▝ …). Other characters fall back to the normal font, so a block body
// can be mixed with ASCII faces. Use for block-art species.
void buddyPrintBlocks(const char* const* lines, uint8_t nLines, int yOffset, uint16_t color, int xOff = 0);

// Set sprite text color directly + cursor (for ad-hoc particle drawing).
void buddySetCursor(int x, int y);
void buddySetColor(uint16_t fg);
void buddyPrint(const char* s);

// ── shared overlay effects ──────────────────────────────────────────────
// The particle overlays below appeared verbatim (modulo a couple of small
// per-species parameters) in 13-19 of the species files. Bodies live in
// src/buddies/overlays.cpp and are copied from the canonical (majority)
// variant byte-for-byte, so a species that calls them with its old local
// values renders pixel-identically. Species with genuinely bespoke
// particle code just keep it inline.

// Dizzy: two '*' orbiting an 8-point ellipse, half a cycle apart.
void overlayOrbitStars(uint32_t t, int yOff = 6,
                       uint16_t c1 = BUDDY_CYAN, uint16_t c2 = BUDDY_YEL);
// Heart: 5 staggered 'v' rising with a phase wiggle.
void overlayHearts(uint32_t t);
// Celebrate: 6 confetti columns falling through the body; glyphs alternate
// between `a` and `b` (most species use "*" / ".").
void overlayConfetti(uint32_t t, const char* a = "*", const char* b = ".");
// Busy: the 6-frame dot ticker next to the body.
void overlayDots(uint32_t t, int x = 22, int y = 14);
// Attention: two '!' blinking at different cadences.
void overlayBang(uint32_t t, int x1 = -4, int x2 = 4, int y2 = 4);

// Shared choreography tables — the dizzy and celebrate beat sequences are
// identical in 18 of 19 species (the shift tables vary a little more, so
// species with bespoke shifts keep theirs locally).
extern const uint8_t BUDDY_DIZZY_SEQ[14];
extern const int8_t  BUDDY_DIZZY_XSHIFT[14];
extern const uint8_t BUDDY_CELEB_SEQ[16];
extern const int8_t  BUDDY_CELEB_YSHIFT[16];
