#include "blockart.h"
#include "hal/display.h"

namespace blockart {

// Decode one UTF-8 sequence at *p, advance *p past it, return the
// codepoint. Malformed bytes are passed through as Latin-1 so the
// renderer never gets stuck.
static uint32_t utf8Next(const char** p) {
  const uint8_t* s = (const uint8_t*)*p;
  uint8_t c = s[0];
  uint32_t cp;
  int n;
  if (c < 0x80)            { cp = c;        n = 1; }
  else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
  else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
  else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 4; }
  else                     { cp = c;        n = 1; }  // stray continuation
  for (int i = 1; i < n; i++) {
    if ((s[i] & 0xC0) != 0x80) { n = i; break; }      // truncated — stop early
    cp = (cp << 6) | (s[i] & 0x3F);
  }
  *p += n;
  return cp;
}

int cellLen(const char* s) {
  int n = 0;
  while (*s) { utf8Next(&s); n++; }
  return n;
}

bool isBlock(uint32_t cp) { return cp >= 0x2580 && cp <= 0x259F; }

// Blend fg toward bg in RGB565. a = weight of fg, 0..255.
static uint16_t blend(uint16_t fg, uint16_t bg, uint8_t a) {
  int r1 = (fg >> 11) & 0x1F, g1 = (fg >> 5) & 0x3F, b1 = fg & 0x1F;
  int r2 = (bg >> 11) & 0x1F, g2 = (bg >> 5) & 0x3F, b2 = bg & 0x1F;
  int r = (r1 * a + r2 * (255 - a)) / 255;
  int g = (g1 * a + g2 * (255 - a)) / 255;
  int b = (b1 * a + b2 * (255 - a)) / 255;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

// Fill the block-element glyph cp inside the cell [x,x+cw) × [y,y+ch).
// The cell is cleared to bg first so partial blocks (halves/quadrants)
// erase whatever the previous frame drew underneath.
static void drawBlockCell(Surface& gfx, uint32_t cp, int x, int y,
                          int cw, int ch, uint16_t fg, uint16_t bg) {
  gfx.fillRect(x, y, cw, ch, bg);

  // Eighth boundaries, snapped exactly to the cell edges (k=0 and k=8).
  auto xk = [&](int k) { return x + k * cw / 8; };
  auto yk = [&](int k) { return y + k * ch / 8; };

  if (cp == 0x2580) {                               // ▀ upper half
    gfx.fillRect(x, y, cw, ch / 2, fg);
  } else if (cp >= 0x2581 && cp <= 0x2588) {        // ▁..█ lower n/8 (█ = full)
    int top = yk(8 - (cp - 0x2580));
    gfx.fillRect(x, top, cw, y + ch - top, fg);
  } else if (cp >= 0x2589 && cp <= 0x258F) {        // ▉..▏ left n/8
    int n = 0x2590 - cp;                            // 7..1
    gfx.fillRect(x, y, xk(n) - x, ch, fg);
  } else if (cp == 0x2590) {                         // ▐ right half
    int xm = xk(4);
    gfx.fillRect(xm, y, x + cw - xm, ch, fg);
  } else if (cp >= 0x2591 && cp <= 0x2593) {         // ░▒▓ shades
    uint8_t a = (cp == 0x2591) ? 64 : (cp == 0x2592) ? 128 : 192;
    gfx.fillRect(x, y, cw, ch, blend(fg, bg, a));
  } else if (cp == 0x2594) {                         // ▔ upper 1/8
    gfx.fillRect(x, y, cw, yk(1) - y, fg);
  } else if (cp == 0x2595) {                         // ▕ right 1/8
    int xm = xk(7);
    gfx.fillRect(xm, y, x + cw - xm, ch, fg);
  } else if (cp >= 0x2596 && cp <= 0x259F) {         // quadrant blocks
    // bit map per codepoint: TL=1, TR=2, BL=4, BR=8
    //  ▖2596 BL  ▗2597 BR  ▘2598 TL  ▙2599 TL+BL+BR  ▚259A TL+BR
    //  ▛259B TL+TR+BL  ▜259C TL+TR+BR  ▝259D TR  ▞259E TR+BL  ▟259F TR+BL+BR
    static const uint8_t M[10] = {4, 8, 1, 13, 9, 7, 11, 2, 6, 14};
    uint8_t m = M[cp - 0x2596];
    int xm = x + cw / 2, ym = y + ch / 2;
    if (m & 1) gfx.fillRect(x,  y,  xm - x,      ym - y,      fg);  // TL
    if (m & 2) gfx.fillRect(xm, y,  x + cw - xm, ym - y,      fg);  // TR
    if (m & 4) gfx.fillRect(x,  ym, xm - x,      y + ch - ym, fg);  // BL
    if (m & 8) gfx.fillRect(xm, ym, x + cw - xm, y + ch - ym, fg);  // BR
  }
}

void drawLine(Surface& gfx, const char* line, int x, int y,
              int scale, uint16_t fg, uint16_t bg) {
  const int cw = 6 * scale, ch = 8 * scale;
  gfx.setTextSize(scale);
  gfx.setTextColor(fg, bg);
  for (const char* p = line; *p; ) {
    uint32_t cp = utf8Next(&p);
    if (isBlock(cp)) {
      drawBlockCell(gfx, cp, x, y, cw, ch, fg, bg);
    } else {
      // Ordinary glyph — the GFX font clears its own cell to bg. Anything
      // outside Latin-1 the classic font can't draw, so show a blank.
      gfx.setCursor(x, y);
      gfx.print((char)(cp <= 0xFF ? cp : ' '));
    }
    x += cw;
  }
}

void drawLineCentered(Surface& gfx, const char* line, int cx, int y,
                      int scale, uint16_t fg, uint16_t bg) {
  int w = cellLen(line) * 6 * scale;
  drawLine(gfx, line, cx - w / 2, y, scale, fg, bg);
}

}  // namespace blockart