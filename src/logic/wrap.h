#pragma once
#include <stdint.h>
#include <string.h>

// Pure, Arduino-free logic — host-testable via `pio test -e native`.
//
// Word-wrap `in` into rows of at most `width` visible columns. Used by the
// HUD, the full-screen transcript and the approval hint. Continuation rows
// get a one-space indent. Words longer than a row are hard-split, with the
// break snapped back to a UTF-8 character boundary so a multi-byte glyph
// isn't cut (which would render as a garbage box).
inline uint8_t wrapInto(const char* in, char out[][48], uint8_t maxRows, uint8_t width) {
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
