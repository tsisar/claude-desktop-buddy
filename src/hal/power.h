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
// original firmware used on the DEVICE info screen and in xfer.h status —
// those readouts (and the cell's charge profile) now live in battery.h.

// Begin the AXP2101 on the already-initialised `w` (shared I2C bus) and
// enable the display rails. Returns false if the PMU isn't found.
bool powerInit(TwoWire& w);
bool powerOk();

// PWRON physical-button events (AXP2101).
//
// The M5 build used M5.Axp.GetBtnPress() returning a 3-state:
//   0 = nothing, 1 = long, 2 = short. The AXP2101 latches short and long
// press into separate IRQ-status bits; powerPollButton() reads them, clears
// them, and returns the same intent in a typed enum. Call from the main
// loop; safe at any rate (the chip latches between calls).
//
// Hardware-level hard-power-off on a sustained hold: powerInit() now arms
// the AXP2101's own long-press → PWROFF (setLongPressPowerOFF + a 6s off
// time), so holding PWRON cuts the rails in hardware even if the firmware
// is wedged. PWRON_LONG can therefore stay unhandled in the loop.
enum PwronEvent : uint8_t { PWRON_NONE = 0, PWRON_SHORT = 1, PWRON_LONG = 2 };
PwronEvent powerPollButton();

// Pet the AXP2101 hardware watchdog. powerInit() arms an 8s watchdog set to
// reset-and-power-cycle the board; the main loop MUST call this every pass
// so a healthy loop never trips it. If the loop ever wedges (I2C stall, BLE
// stall, display desync) the watchdog fires and the board recovers on its
// own — no battery pull. Safe no-op until powerInit() succeeds.
void powerFeedWatchdog();

// Screen on/off is NOT a rail toggle on this board: the panel runs on the
// AXP2101 power-on defaults (see docs/device-power-map.md), and ALDO1 is the
// ES8311 codec rail — cutting it kills audio. Blank the screen with
// Surface::setBrightness(0) (the `screenOn` path in main.cpp), not a rail gate.

// Dump every rail (voltage + on/off) to Serial — bring-up diagnostic.
void powerDumpRails();
