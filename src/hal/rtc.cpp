#include "rtc.h"
#include "../board_pins.h"
#include <Arduino.h>
#include <Wire.h>
#include <SensorPCF85063.hpp>

// PCF85063-backed RTC HAL. See rtc.h for the why.

static SensorPCF85063 rtc;
static bool           ok = false;

bool rtcInit(TwoWire& w) {
  if (ok) return true;
  // The library's init() takes the bus + pins + addr; the bus is already
  // up by the time we get here so this just attaches the driver.
  if (!rtc.init(w, IIC_SDA, IIC_SCL, RTC_ADDR)) {
    Serial.printf("[rtc] PCF85063 not found at 0x%02X\n", RTC_ADDR);
    return false;
  }
  ok = true;
  Serial.println("[rtc] PCF85063 ok");
  return true;
}

bool rtcOk() { return ok; }

bool rtcGetTime(RtcTime* t) {
  if (!ok) return false;
  RTC_DateTime dt = rtc.getDateTime();
  if (!dt.available) return false;
  t->Hours   = dt.hour;
  t->Minutes = dt.minute;
  t->Seconds = dt.second;
  return true;
}

bool rtcGetDate(RtcDate* d) {
  if (!ok) return false;
  RTC_DateTime dt = rtc.getDateTime();
  if (!dt.available) return false;
  d->Year    = dt.year;
  d->Month   = dt.month;
  d->Date    = dt.day;
  d->WeekDay = dt.week;
  return true;
}

void rtcSetDateTime(const RtcDate& d, const RtcTime& t) {
  if (!ok) return;
  rtc.setDateTime(d.Year, d.Month, d.Date, t.Hours, t.Minutes, t.Seconds);
}