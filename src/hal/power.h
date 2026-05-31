#pragma once
#include <stdint.h>
class TwoWire;

// AXP2101 PMU HAL for the Waveshare ESP32-S3-Touch-AMOLED-1.8.
//
// Why this exists: the AMOLED panel's power rails are gated by the AXP2101,
// not wired to permanent 3V3. Verified on hardware — at reset the board
// leaves ALDO1 (1.8V) and ALDO3 (3.3V) *configured but disabled*; the panel
// stays dark until they are enabled. powerInit() turns them on, so it MUST
// run (after Wire.begin) BEFORE the display is initialised.
//
// This also replaces the M5StickC Plus M5.Axp.* battery/USB readouts the
// original firmware used on the DEVICE info screen and in xfer.h status.

// Begin the AXP2101 on the already-initialised `w` (shared I2C bus) and
// enable the display rails. Returns false if the PMU isn't found.
bool powerInit(TwoWire& w);
bool powerOk();

// Battery / charger telemetry (AXP2101). Safe to call when powerOk() is
// false — they return 0 / false.
int  batteryMilliVolts();   // 0 if unknown
int  batteryPercent();      // 0..100, 0 if unknown
bool onUsb();               // VBUS present
bool charging();            // actively charging the cell

// Dump every rail (voltage + on/off) to Serial — bring-up diagnostic.
void powerDumpRails();
