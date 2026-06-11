#include "clock.h"
#include "board_pins.h"   // LCD_WIDTH / LCD_HEIGHT
#include "hal/display.h"  // Surface
#include "hal/rtc.h"      // RtcTime/RtcDate, rtcGetTime/rtcGetDate
#include <Arduino.h>

// Purely presentational: the caller (main.cpp) owns the "should this show"
// policy — settings().battery + dataRtcValid() — because that state lives in
// data.h/stats.h, which are file-static singletons valid only in main.cpp's
// translation unit. We must NOT include them here or we'd read a second,
// always-default copy (that bug hid the widget). We only read the RTC HAL
// (a real .cpp) and draw.
//
// Shared render surface, defined in main.cpp (same extern pattern as
// character.cpp). The battery widget also lives in main.cpp's render layer;
// the full face overlays it top-right.
extern Surface gfx;
void drawBatteryWidget(int bx, int by);

// Screen-edge inset, matching main.cpp's SAFE.
static const int SAFE = 4;

static const char* const MON[] = {
  "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
};
static const char* const DOW[] = { "Sun","Mon","Tue","Wed","Thu","Fri","Sat" };

void clockDrawWidget(int x, int y) {
  RtcTime tm;
  if (!rtcGetTime(&tm)) return;
  char hm[6]; snprintf(hm, sizeof(hm), "%02u:%02u", tm.Hours, tm.Minutes);
  gfx.setTextSize(2);
  gfx.setTextColor(0xFFFF, 0x0000);
  gfx.setTextDatum(ML_DATUM);
  gfx.drawString(hm, x, y);
  gfx.setTextDatum(TL_DATUM);
}

void clockDrawFace() {
  const int W = LCD_WIDTH, H = LCD_HEIGHT;
  gfx.fillSprite(0x0000);
  RtcTime tm; RtcDate dt;
  if (!rtcGetTime(&tm) || !rtcGetDate(&dt)) return;

  // AMOLED anti-burn-in: nudge the whole face (and the battery widget) by
  // a few pixels on a slow cycle driven by the minutes, so the parked face
  // — which on USB power stays up for days — never drives the exact same
  // pixels for hours. Full ±4 px pattern repeats hourly; invisible at a
  // glance, decisive for the SH8601's lifetime.
  int dx = (int)(tm.Minutes % 8) - 4;
  int dy = (int)((tm.Minutes / 8) % 8) - 4;

  char hm[6]; snprintf(hm, sizeof(hm), "%02u:%02u", tm.Hours, tm.Minutes);
  // Seconds on their own row — drop the leading colon (it only made sense
  // when they used to sit inline after the minutes).
  char ss[3]; snprintf(ss, sizeof(ss), "%02u", tm.Seconds);
  uint8_t mi = (dt.Month >= 1 && dt.Month <= 12) ? dt.Month - 1 : 0;
  char dl[20];
  snprintf(dl, sizeof(dl), "%s %s %02u",
           DOW[dt.WeekDay % 7], MON[mi], dt.Date);

  gfx.setTextDatum(MC_DATUM);
  gfx.setTextSize(8);
  gfx.setTextColor(0xFFFF, 0x0000);
  gfx.drawString(hm, W / 2 + dx, H / 2 - 40 + dy);

  gfx.setTextSize(4);
  gfx.setTextColor(0xC618, 0x0000);
  gfx.drawString(ss, W / 2 + dx, H / 2 + 50 + dy);

  gfx.setTextSize(3);
  gfx.drawString(dl, W / 2 + dx, H / 2 + 110 + dy);
  gfx.setTextDatum(TL_DATUM);

  drawBatteryWidget(W - 14 - 14 - SAFE + dx, 14 + SAFE + dy);   // top-right
}