#pragma once
#include <stdint.h>

// Surface — the slice of the TFT_eSPI / TFT_eSprite API that the buddy
// firmware actually calls, reimplemented on top of Arduino_GFX's
// Arduino_Canvas (a full-screen offscreen framebuffer in PSRAM).
//
// The M5 build draws into a `TFT_eSprite spr` and `spr.pushSprite(0,0)` to
// the LCD. The port keeps the exact same call sites by drawing into a
// `Surface gfx` whose method names mirror TFT_eSprite:
//
//   M5 (TFT_eSprite)            Surface (Arduino_Canvas)
//   ----------------            ------------------------
//   spr.createSprite(w,h)       gfx.createSprite(w,h)   // alloc canvas
//   spr.fillSprite(c)           gfx.fillSprite(c)       // == fillScreen
//   spr.pushSprite(0,0)         gfx.pushSprite()        // == canvas flush
//   spr.fillRect/drawRect/...   same names, forwarded
//   spr.setTextDatum/drawString same names, centering done here
//
// Color is RGB565, same as TFT_eSPI. Geometry is whatever the canvas was
// created with (368x448 on this board).
//
// Stage 2 only needs construct + begin + a few primitives; the full method
// set is declared now so stage 3 (porting main.cpp) is a mechanical swap.

// TFT_eSPI text-datum constants, mirrored so existing call sites compile.
enum {
  TL_DATUM = 0,  // top-left
  TC_DATUM = 1,  // top-centre
  TR_DATUM = 2,  // top-right
  ML_DATUM = 3,
  MC_DATUM = 4,  // middle-centre
  MR_DATUM = 5,
  BL_DATUM = 6,
  BC_DATUM = 7,
  BR_DATUM = 8,
};

class Arduino_Canvas;   // fwd-decl; impl includes the real header
class Arduino_SH8601;

class Surface {
public:
  Surface() = default;

  // Bring up the SH8601 panel + allocate the PSRAM canvas. Returns false if
  // the panel init or the canvas allocation failed. Call once in setup().
  bool begin();

  // TFT_eSprite-compatible surface ops -----------------------------------
  void createSprite(int16_t w, int16_t h);   // no-op if begin() already sized
  void fillSprite(uint16_t color);           // fill whole canvas
  void pushSprite(int16_t x = 0, int16_t y = 0);  // flush canvas to panel

  int16_t width() const;
  int16_t height() const;

  // Primitives (forward to Arduino_GFX) ----------------------------------
  void drawPixel(int16_t x, int16_t y, uint16_t color);
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
  void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
  void fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color);
  void drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color);
  void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color);
  void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color);
  void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color);
  void fillCircle(int16_t x, int16_t y, int16_t r, uint16_t color);
  void drawCircle(int16_t x, int16_t y, int16_t r, uint16_t color);
  void fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint16_t color);

  // Text -----------------------------------------------------------------
  void setTextColor(uint16_t fg);
  void setTextColor(uint16_t fg, uint16_t bg);
  void setTextSize(uint8_t s);
  void setTextSize(uint8_t sx, uint8_t sy, uint8_t margin = 0);
  void setCursor(int16_t x, int16_t y);
  void setTextDatum(uint8_t datum);          // affects drawString() only
  void print(const char* s);
  void print(char c);
  void printf(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
  // Datum-aware string draw (TFT_eSPI semantics; Arduino_GFX lacks this).
  void drawString(const char* s, int16_t x, int16_t y);

  // U8g2 font support — pass a u8g2 font byte array (Adafruit GFXfont
  // overload also exists in Arduino_GFX, this one is the u8g2 path).
  // Set null to fall back to the built-in 6×8 ASCII font.
  void setFont(const uint8_t* u8g2Font);
  // Tell the GFX print() pipeline to decode UTF-8 multi-byte sequences
  // and look the codepoints up in the active font. Default off (ASCII).
  void setUTF8Print(bool enable);

  // Panel-level (not on the canvas in TFT_eSPI, but handy): 0..255.
  void setBrightness(uint8_t b);

  // Raw canvas for the GIF decoder's per-pixel draw callback.
  Arduino_Canvas* raw() { return _cv; }

  // The SH8601 panel itself, for bring-up diagnostics that need to draw
  // straight to the glass (bypassing the offscreen canvas).
  Arduino_SH8601* panel() { return _pnl; }

private:
  Arduino_Canvas* _cv  = nullptr;
  Arduino_SH8601* _pnl = nullptr;
  uint8_t  _datum = TL_DATUM;
  // Arduino_GFX exposes no text-size getter, so Surface mirrors it for the
  // datum math in drawString(). Kept in sync by setTextSize().
  uint8_t  _tsx = 1, _tsy = 1;
  // 6x8 base glyph cell, like TFT_eSPI font 1.
  static constexpr int GLYPH_W = 6;
  static constexpr int GLYPH_H = 8;
};
