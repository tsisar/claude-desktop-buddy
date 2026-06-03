#include "clock.h"
#include "board_pins.h"   // LCD_WIDTH / LCD_HEIGHT
#include "hal/display.h"  // Surface
#include "hal/rtc.h"      // RtcTime/RtcDate, rtcGetTime/rtcGetDate
#include "data.h"         // dataRtcValid(); pulls stats.h for settings()
#include <Arduino.h>

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
  if (!settings().battery) return;
  RtcTime tm;
  if (!dataRtcValid() || !rtcGetTime(&tm)) return;
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
  gfx.drawString(hm, W / 2, H / 2 - 40);

  gfx.setTextSize(4);
  gfx.setTextColor(0xC618, 0x0000);
  gfx.drawString(ss, W / 2, H / 2 + 50);

  gfx.setTextSize(3);
  gfx.drawString(dl, W / 2, H / 2 + 110);
  gfx.setTextDatum(TL_DATUM);

  drawBatteryWidget(W - 14 - 14 - SAFE, 14 + SAFE);   // top-right
}