#include "power.h"
#include "expander.h"
#include "../board_pins.h"
#include <Arduino.h>
#include <Wire.h>
#include <XPowersLib.h>

// AXP2101-backed power HAL. See power.h for the why.

static XPowersPMU pmu;
static bool       ok = false;

// Battery charge current. The installed cell is 350 mAh. LiPo best practice is
// to charge at ~0.5C for longevity (0.5–1C is the safe band; >1C shortens life,
// and the vendor's 400 mA / upstream's 500 mA are ~1.1–1.4C here — too hot for
// this pack). 0.5C of 350 mAh = 175 mA, which the AXP2101 supports exactly.
// Drop to _150MA for an even cooler/gentler charge; never go above _350MA (1C).
static constexpr uint8_t CHARGE_CURRENT = XPOWERS_AXP2101_CHG_CUR_175MA;

bool powerInit(TwoWire& w) {
  ok = pmu.begin(w, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (!ok) {
    Serial.println("[pmu] AXP2101 not found at 0x34");
    return false;
  }
  Serial.printf("[pmu] AXP2101 ok (chipID 0x%02X)\n", pmu.getChipID());

  // These two writes only RE-ASSERT the power-on defaults: at reset every
  // AXP2101 rail is already ON at a sane voltage and the board boots fully on
  // defaults with zero PMU config (see docs/device-power-map.md §2). We pin
  // them explicitly because ALDO1 is the *verified* ES8311 codec rail (3.3 V
  // AVDD) — the old guess that it was a 1.8 V display rail set it to 1.8 V and
  // MUTED the codec with no recovery short of a power-cycle. Never lower ALDO1.
  // ALDO3's consumer is still unproven — it is NOT the display rail despite the
  // old name; 3.0 V here just restores its default. Full map + confidence tags:
  // docs/device-power-map.md.
  pmu.setALDO1Voltage(3300);   // ES8311 AVDD — verified; never lower
  pmu.enableALDO1();
  pmu.setALDO3Voltage(3000);   // consumer unproven; re-assert default only
  pmu.enableALDO3();

  // Touch + display + one more line on this board are held in reset by the
  // XCA9554 expander at 0x20 (verified vs. the vendor 04_GFX_FT3168_Image
  // example). Pulse them out of reset NOW — after the rails are up and
  // before the display init / touch probe. Without this, FT3168 misses
  // its first I2C ACK on a cold cycle (the AXP2101 rail toggle above
  // counts as a cold cycle for everything downstream).
  expanderInit(w);
  expanderResetPulse();

  // Battery fuel-gauge + ADC channels, so the telemetry getters below work.
  pmu.enableBattDetection();              // PMU must know a cell is present...
  pmu.enableBattVoltageMeasure();
  pmu.enableVbusVoltageMeasure();
  pmu.enableSystemVoltageMeasure();

  // Charger configuration. Without this the AXP2101 runs on whatever its
  // registers happened to hold, which is how a "100%" cell could drain flat
  // on a 30-min trip and then refuse to boot from battery alone (deep
  // discharge → only VBUS can re-trigger power-on).
  pmu.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);   // 4.2V cut-off
  pmu.setPrechargeCurr(XPOWERS_AXP2101_PRECHARGE_50MA);
  pmu.setChargerConstantCurr(CHARGE_CURRENT);
  pmu.setChargerTerminationCurr(XPOWERS_AXP2101_CHG_ITERM_25MA);
  // Cap how much we pull from the USB port so a weak source stays stable.
  pmu.setVbusCurrentLimit(XPOWERS_AXP2101_VBUS_CUR_LIM_500MA);

  // Under-voltage protection. If the rail sags this low the PMU pulls the
  // plug rather than browning out the ESP32 mid-write.
  pmu.setSysPowerDownVoltage(2600);
  pmu.setLowBatWarnThreshold(15);         // %, fires the low-batt IRQ
  pmu.setLowBatShutdownThreshold(5);      // %, PMU auto-shuts down here

  // Enable the coulomb-counter fuel gauge. THIS is what makes
  // getBatteryPercent() report the real charge — without it the gauge
  // register holds its power-on default (100), so every cell reads full.
  pmu.fuelGaugeControl(true, true);

  // PWRON IRQ — short / long press latched in the chip's status reg so the
  // main loop can poll powerPollButton() at any rate without losing events.
  pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
  pmu.clearIrqStatus();
  pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ |
                XPOWERS_AXP2101_PKEY_LONG_IRQ);
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

PwronEvent powerPollButton() {
  if (!ok) return PWRON_NONE;
  pmu.getIrqStatus();
  PwronEvent ev = PWRON_NONE;
  // Long takes priority: if both bits latched between polls (unlikely but
  // possible on a long press followed by a release within one tick), the
  // intent that matters is "user wants power off / hard action".
  if      (pmu.isPekeyLongPressIrq())  ev = PWRON_LONG;
  else if (pmu.isPekeyShortPressIrq()) ev = PWRON_SHORT;
  pmu.clearIrqStatus();
  return ev;
}

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