#include "display.h"
#include "../board_pins.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <stdarg.h>

// Arduino_GFX backing for Surface. The panel is an SH8601 driven over QSPI;
// we draw into an Arduino_Canvas (full-screen RGB565 framebuffer in PSRAM)
// and flush() it to the panel — exactly the TFT_eSprite model.

bool Surface::begin() {
  // QSPI data bus: CS, SCLK, D0..D3 (from board_pins.h, verbatim Waveshare).
  Arduino_DataBus* bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

  // SH8601, no reset GPIO (GFX_NOT_DEFINED), rotation 0, 368x448.
  _pnl = new Arduino_SH8601(bus, GFX_NOT_DEFINED, 0, LCD_WIDTH, LCD_HEIGHT);

  // Initialise the panel EXPLICITLY first, exactly like Waveshare's
  // 01_HelloWorld (Arduino_SH8601 + gfx->begin()). Relying on
  // Arduino_Canvas::begin() to do this left the glass dark on both the
  // direct and canvas paths — the panel's display-on/init burst wasn't
  // running. Begin it ourselves, push it on, set brightness.
  if (!_pnl->begin()) {
    return false;
  }
  _pnl->fillScreen(0x0000);
  _pnl->setBrightness(255);

  // Offscreen canvas: LCD_WIDTH*LCD_HEIGHT*2 bytes (~322KB) in PSRAM
  // (BOARD_HAS_PSRAM). The panel is already begun, so this is just the
  // framebuffer the firmware draws into and flush()es to the live panel.
  _cv = new Arduino_Canvas(LCD_WIDTH, LCD_HEIGHT, _pnl);
  if (!_cv->begin()) {
    return false;
  }
  _cv->fillScreen(0x0000);
  _cv->flush();
  return true;
}

void Surface::createSprite(int16_t, int16_t) {
  // No-op: the canvas is sized at begin(). Present so the M5 call site
  // (spr.createSprite(W,H)) ports without edits.
}

void Surface::fillSprite(uint16_t color) { _cv->fillScreen(color); }

void Surface::pushSprite(int16_t, int16_t) { _cv->flush(); }

int16_t Surface::width() const  { return _cv ? _cv->width()  : LCD_WIDTH; }
int16_t Surface::height() const { return _cv ? _cv->height() : LCD_HEIGHT; }

void Surface::drawPixel(int16_t x, int16_t y, uint16_t c) { _cv->drawPixel(x, y, c); }
void Surface::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) { _cv->fillRect(x, y, w, h, c); }
void Surface::drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) { _cv->drawRect(x, y, w, h, c); }
void Surface::fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t c) { _cv->fillRoundRect(x, y, w, h, r, c); }
void Surface::drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t c) { _cv->drawRoundRect(x, y, w, h, r, c); }
void Surface::drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t c) { _cv->drawFastHLine(x, y, w, c); }
void Surface::drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t c) { _cv->drawFastVLine(x, y, h, c); }
void Surface::drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t c) { _cv->drawLine(x0, y0, x1, y1, c); }
void Surface::fillCircle(int16_t x, int16_t y, int16_t r, uint16_t c) { _cv->fillCircle(x, y, r, c); }
void Surface::drawCircle(int16_t x, int16_t y, int16_t r, uint16_t c) { _cv->drawCircle(x, y, r, c); }
void Surface::fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint16_t c) { _cv->fillTriangle(x0, y0, x1, y1, x2, y2, c); }

void Surface::setTextColor(uint16_t fg) { _cv->setTextColor(fg); }
void Surface::setTextColor(uint16_t fg, uint16_t bg) { _cv->setTextColor(fg, bg); }
void Surface::setTextSize(uint8_t s) { _tsx = _tsy = s; _cv->setTextSize(s); }
void Surface::setTextSize(uint8_t sx, uint8_t sy, uint8_t margin) { _tsx = sx; _tsy = sy; _cv->setTextSize(sx, sy, margin); }
void Surface::setCursor(int16_t x, int16_t y) { _cv->setCursor(x, y); }
void Surface::setTextDatum(uint8_t datum) { _datum = datum; }
void Surface::print(const char* s) { _cv->print(s); }
void Surface::print(char c) { _cv->print(c); }

void Surface::printf(const char* fmt, ...) {
  char b[128];
  va_list a; va_start(a, fmt);
  vsnprintf(b, sizeof(b), fmt, a);
  va_end(a);
  _cv->print(b);
}

// TFT_eSPI's drawString honours the text datum (anchor point). Arduino_GFX
// only has cursor-based text, so compute the top-left origin from the datum
// and the 6x8*textsize glyph box, then print.
void Surface::drawString(const char* s, int16_t x, int16_t y) {
  int len = 0; for (const char* p = s; *p; p++) len++;
  int w = len * GLYPH_W * _tsx;
  int h = GLYPH_H * _tsy;

  int ox = x, oy = y;
  switch (_datum) {
    case TC_DATUM: ox = x - w / 2; break;
    case TR_DATUM: ox = x - w;     break;
    case ML_DATUM: oy = y - h / 2; break;
    case MC_DATUM: ox = x - w / 2; oy = y - h / 2; break;
    case MR_DATUM: ox = x - w;     oy = y - h / 2; break;
    case BL_DATUM: oy = y - h;     break;
    case BC_DATUM: ox = x - w / 2; oy = y - h; break;
    case BR_DATUM: ox = x - w;     oy = y - h; break;
    default: break;  // TL_DATUM
  }
  _cv->setCursor(ox, oy);
  _cv->print(s);
}

void Surface::setBrightness(uint8_t b) { if (_pnl) _pnl->setBrightness(b); }
