#pragma once

// Battery & charger telemetry, backed by the AXP2101 PMU.
//
// Split out of power.cpp: power.h owns bringing the PMU up (display rails,
// PWRON button); this owns everything about the *cell* — the charge profile
// and the runtime readouts the home/DEVICE UI and xfer.h status report.
// Both halves share the one AXP2101 instance (see pmu.h). The getters are
// safe to call before the PMU is up — they return 0 / false while
// powerOk() is false.

// One-shot charger + fuel-gauge configuration for the installed cell.
// Called from powerInit() once the AXP2101 is alive — not for direct use.
void batteryConfigCharger();

int  batteryMilliVolts();   // 0 if unknown
int  batteryPercent();      // 0..100, 0 if unknown
bool onUsb();               // VBUS present
bool charging();            // actively charging the cell