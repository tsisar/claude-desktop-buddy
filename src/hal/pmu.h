#pragma once
#include <XPowersLib.h>

// Shared AXP2101 instance, owned and initialised by power.cpp (powerInit).
// Internal to the power/battery HAL pair only — public code talks to the
// chip through power.h (rails, PWRON button) or battery.h (cell telemetry,
// charger), never this header. It exists so battery.cpp can read the cell
// through the *same* live PMU object that power.cpp brought up, without
// dragging the heavy XPowersLib include into power.h's public callers.
XPowersPMU& axp();