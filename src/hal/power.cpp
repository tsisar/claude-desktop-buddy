#include "power.h"
#include "expander.h"
#include "../board_pins.h"
#include <Arduino.h>
#include <Wire.h>
#include <XPowersLib.h>

// AXP2101-backed power HAL. See power.h for the why.

static XPowersPMU pmu;
static bool       ok = false;

// Battery charge current. AXP2101 defaults are NOT safe to assume, so we set
// this explicitly. Rule of thumb: ~0.5C of the cell's capacity. 150mA suits a
// small (~300mAh) buddy LiPo and is safe-but-slow for anything larger — bump
// to _300MA / _500MA once the installed cell's capacity is confirmed.
static constexpr uint8_t CHARGE_CURRENT = XPOWERS_AXP2101_CHG_CUR_150MA;

bool powerInit(TwoWire& w) {
  ok = pmu.begin(w, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (!ok) {
    Serial.println("[pmu] AXP2101 not found at 0x34");
    return false;
  }
  Serial.printf("[pmu] AXP2101 ok (chipID 0x%02X)\n", pmu.getChipID());

  // DO NOT touch ALDO1/ALDO3 here. Measured AXP2101 power-on defaults on this
  // board: every rail is already ON at a sane voltage (DC1 3.3V, ALDO1 3.3V,
  // ALDO2 3.3V, ALDO3 3.0V, ALDO4 1.8V, BLDO1 1.2V, BLDO2 2.8V).
  //
  // ALDO1 (default 3.3V) powers the ES8311 audio codec. The old
  // setALDO1Voltage(1800)+enableALDO1() was the audio-killer: it glitched the
  // rail AND under-volted the codec to 1.8V, so the speaker stayed silent.
  // The earlier "ALDO1 = 1.8V AMOLED rail" comment was wrong. Leaving the
  // rails at their defaults = working audio + working display.
  //   pmu.setALDO1Voltage(1800); pmu.enableALDO1();   // ← killed audio, removed
  //   pmu.setALDO3Voltage(3300); pmu.enableALDO3();

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

// TODO(audio/display): this is the SECOND audio landmine. ALDO1 powers the
// ES8311 codec, so disableALDO1() on screen-off (menu "turn off",
// main.cpp:1035) cuts codec power → audio dies and does NOT recover on wake
// (codec loses its config; nothing re-inits it). Rework screen on/off to use
// the SH8601 panel (applyBrightness(0) / display sleep), which is what the
// auto-idle path already uses — do NOT toggle ALDO1/ALDO3 here.
// For now it still cuts the rails; fix when reworking the power/display flow.
//
// AUDIO INVESTIGATION FINDINGS (so they aren't lost):
//   • Root cause of "no sound": powerInit's setALDO1Voltage(1800) (above) —
//     ALDO1 (default 3.3V) feeds the ES8311; lowering/glitching it muted it.
//   • All AXP rails + charger (4.2V / 150mA / SysPDn 2.6V) are already at the
//     wanted values by default → the charge/sys setters in powerInit are
//     redundant no-ops (kept only for cross-unit robustness).
//   • XCA9554 expander pulse (pins 0/1/2) does NOT affect audio (ruled out).
//   • ES8311 warm-boot: after a soft reset the codec keeps its powered analog
//     state and a plain re-init leaves it silent until a hard power-cycle —
//     es8311_init needs a delay after the 0x1F reset + after 0x80 power-on.
//   • Verified-audible audio backend = Arduino ESP_I2S (I2SClass) at MCLK x256
//     (Waveshare 15_ES8311). The current raw ESP-IDF i2s_std x384 path in
//     audio.cpp is unverified — port/verify the ESP_I2S x256 version.
void powerSetDisplay(bool on) {
  if (!ok) return;
  if (on) { pmu.enableALDO1();  pmu.enableALDO3();  }
  else    { pmu.disableALDO1(); pmu.disableALDO3(); }
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
