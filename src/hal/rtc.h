#pragma once
#include <stdint.h>
class TwoWire;

// PCF85063 RTC HAL for the Waveshare ESP32-S3-Touch-AMOLED-1.8.
//
// Mirrors the slice of the M5StickC Plus M5.Rtc API the firmware uses:
// GetTime / SetTime / GetDate / SetDate against two field-named structs.
// Keeping the field names (Hours/Minutes/Seconds, Year/Month/Date/WeekDay)
// identical to M5's RTC_TimeTypeDef / RTC_DateTypeDef so the clock face
// code in main.cpp and the bridge-time-sync path in data.h port verbatim.
//
// Backed by lewisxhe/SensorLib's SensorPCF85063 (its own RTC_DateTime has
// different field names; we copy across).

struct RtcTime {
  uint8_t Hours;     // 0..23
  uint8_t Minutes;   // 0..59
  uint8_t Seconds;   // 0..59
};

struct RtcDate {
  uint16_t Year;     // full year, e.g. 2026
  uint8_t  Month;    // 1..12
  uint8_t  Date;     // 1..31  (M5 calls this "Date", not "Day")
  uint8_t  WeekDay;  // 0..6   (0 = Sunday on M5; PCF85063 matches)
};

// Bring up the PCF85063 on the already-initialised `w` (shared bus, SDA=15
// SCL=14). Returns false if the chip is missing. Safe to re-call.
bool rtcInit(TwoWire& w);
bool rtcOk();

// Read clock. Returns false on I2C error (outputs untouched in that case).
bool rtcGetTime(RtcTime* t);
bool rtcGetDate(RtcDate* d);

// Set clock. Used by the bridge time-sync path (desktop pushes wall time).
void rtcSetDateTime(const RtcDate& d, const RtcTime& t);