#pragma once
#include <stdint.h>

// Block-art renderer — draws monospace UTF-8 art where the Unicode
// "Block Elements" range (U+2580–U+259F) is rasterised as seamless
// rectangle fills instead of font glyphs.
//
// Why not a font: integer-scaled bitmap fonts leave a 1-px advance gap
// between glyph cells, so adjacent █ never touch — block art ends up
// gappy. Filling the full cell with fillRect() tiles perfectly at any
// scale and lets each quadrant/half take its own colour later.
//
// The cell is sized exactly like the classic GFX text cell so block art
// can be mixed character-for-character with ordinary ASCII faces:
//   cellW = 6 * scale,  cellH = 8 * scale,  font size = scale.
// Any non-block codepoint falls back to the GFX font (with bg fill), so
// ".-/\\()" eyes/mouths render just as before inside a block body.
//
// Geometry is given in pixels; the module is standalone and only needs a
// Surface to draw into (see hal/display.h).

class Surface;

namespace blockart {

// Number of UTF-8 codepoints in s (NOT bytes) — use for layout/centering.
int cellLen(const char* s);

// True if cp is a Block Elements glyph this module fills directly.
bool isBlock(uint32_t cp);

// Draw one art line, top-left cell origin at (x, y). Each cell is
// 6*scale wide × 8*scale tall. Block codepoints are filled; others go to
// the GFX font. bg is used to clear each cell so old frames don't ghost.
void drawLine(Surface& gfx, const char* line, int x, int y,
              int scale, uint16_t fg, uint16_t bg);

// Same, but the line is horizontally centered on pixel column cx (cell
// count × cellW, halved). Matches buddyPrintLine's per-line centering.
void drawLineCentered(Surface& gfx, const char* line, int cx, int y,
                      int scale, uint16_t fg, uint16_t bg);

}  // namespace blockart