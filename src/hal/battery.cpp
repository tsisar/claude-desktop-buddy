#include "battery.h"
#include "pmu.h"
#include "power.h"        // powerOk()
#include <Arduino.h>

// AXP2101 battery/charger module. The PMU instance is owned by power.cpp;
// we reach it through axp(). See battery.h for the power.h / battery.h split.

// Battery charge current. The installed cell is 350 mAh. LiPo best practice is
// to charge at ~0.5C for longevity (0.5–1C is the safe band; >1C shortens life,
// and the vendor's 400 mA / upstream's 500 mA are ~1.1–1.4C here — too hot for
// this pack). 0.5C of 350 mAh = 175 mA, which the AXP2101 supports exactly.
// Drop to _150MA for an even cooler/gentler charge; never go above _350MA (1C).
static constexpr uint8_t CHARGE_CURRENT = XPOWERS_AXP2101_CHG_CUR_175MA;

void batteryConfigCharger() {
  // Battery fuel-gauge + ADC channels, so the telemetry getters below work.
  axp().enableBattDetection();              // PMU must know a cell is present...
  axp().enableBattVoltageMeasure();
  axp().enableVbusVoltageMeasure();
  axp().enableSystemVoltageMeasure();

  // Charger configuration. Without this the AXP2101 runs on whatever its
  // registers happened to hold, which is how a "100%" cell could drain flat
  // on a 30-min trip and then refuse to boot from battery alone (deep
  // discharge → only VBUS can re-trigger power-on).
  axp().setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);   // 4.2V cut-off
  axp().setPrechargeCurr(XPOWERS_AXP2101_PRECHARGE_50MA);
  axp().setChargerConstantCurr(CHARGE_CURRENT);
  axp().setChargerTerminationCurr(XPOWERS_AXP2101_CHG_ITERM_25MA);
  // Cap how much we pull from the USB port so a weak source stays stable.
  axp().setVbusCurrentLimit(XPOWERS_AXP2101_VBUS_CUR_LIM_500MA);

  // Under-voltage protection / discharge floor. This is the hard VSYS cutoff:
  // when the system rail (≈ cell voltage on battery) sags below it the PMU
  // pulls the plug. 3.3 V, NOT the chip's 2.6 V minimum: a LiPo must never go
  // below 3.0 V (irreversible capacity loss), and 3.3 V keeps a longevity
  // margin. The cost is ~nil runtime — there's almost no usable capacity
  // below 3.3 V (the discharge-curve "knee"). It also backstops the fuel
  // gauge: the coulomb-counter lies on a fresh/uncalibrated 350 mAh cell, so
  // the 5%-SoC shutdown below can't be trusted alone to stop a deep drain.
  // Range is 2600–3300 mV in 100 mV steps; 3300 is the safe maximum here.
  axp().setSysPowerDownVoltage(3300);
  axp().setLowBatWarnThreshold(15);         // %, fires the low-batt IRQ
  axp().setLowBatShutdownThreshold(5);      // %, PMU auto-shuts down here

  // Enable the coulomb-counter fuel gauge. THIS is what makes
  // getBatteryPercent() report the real charge — without it the gauge
  // register holds its power-on default (100), so every cell reads full.
  axp().fuelGaugeControl(true, true);
}

int batteryMilliVolts() {
  if (!powerOk()) return 0;
  return (int)axp().getBattVoltage();        // mV
}

int batteryPercent() {
  if (!powerOk()) return 0;
  int pct = axp().getBatteryPercent();       // -1 if not yet known
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  return pct;
}

bool onUsb()    { return powerOk() && axp().isVbusIn(); }
bool charging() { return powerOk() && axp().isCharging(); }