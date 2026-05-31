#include "power.h"
#include "../board_pins.h"
#include <Arduino.h>
#include <Wire.h>
#include <XPowersLib.h>

// AXP2101-backed power HAL. See power.h for the why.

static XPowersPMU pmu;
static bool       ok = false;

bool powerInit(TwoWire& w) {
  ok = pmu.begin(w, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (!ok) {
    Serial.println("[pmu] AXP2101 not found at 0x34");
    return false;
  }
  Serial.printf("[pmu] AXP2101 ok (chipID 0x%02X)\n", pmu.getChipID());

  // Verified on this board: ALDO1=1.8V and ALDO3=3.3V are the AMOLED rails,
  // left configured-but-off at reset. Enable them at their own default
  // voltages — never higher — so nothing can be over-volted.
  pmu.setALDO1Voltage(1800);
  pmu.enableALDO1();
  pmu.setALDO3Voltage(3300);
  pmu.enableALDO3();

  // Battery fuel-gauge + ADC channels, so the telemetry getters below work.
  pmu.enableBattVoltageMeasure();
  pmu.enableVbusVoltageMeasure();
  pmu.enableSystemVoltageMeasure();
  return true;
}

bool powerOk() { return ok; }

int batteryMilliVolts() {
  if (!ok) return 0;
  return (int)pmu.getBattVoltage();        // mV
}

int batteryPercent() {
  if (!ok) return 0;
  int pct = pmu.getBatteryPercent();       // -1 if not yet known
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  return pct;
}

bool onUsb()    { return ok && pmu.isVbusIn(); }
bool charging() { return ok && pmu.isCharging(); }

void powerDumpRails() {
  if (!ok) { Serial.println("[pmu] (not initialised)"); return; }
  auto d = [](const char* n, uint16_t mv, bool en) {
    Serial.printf("[pmu]   %-6s %4umV %s\n", n, mv, en ? "ON" : "off");
  };
  d("DC1",   pmu.getDC1Voltage(),   pmu.isEnableDC1());
  d("ALDO1", pmu.getALDO1Voltage(), pmu.isEnableALDO1());
  d("ALDO2", pmu.getALDO2Voltage(), pmu.isEnableALDO2());
  d("ALDO3", pmu.getALDO3Voltage(), pmu.isEnableALDO3());
  d("ALDO4", pmu.getALDO4Voltage(), pmu.isEnableALDO4());
  d("BLDO1", pmu.getBLDO1Voltage(), pmu.isEnableBLDO1());
  d("BLDO2", pmu.getBLDO2Voltage(), pmu.isEnableBLDO2());
}
