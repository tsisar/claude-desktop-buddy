#include "power.h"
#include "pmu.h"
#include "battery.h"
#include "expander.h"
#include "../board_pins.h"
#include <Arduino.h>
#include <Wire.h>
#include <XPowersLib.h>

// AXP2101-backed power HAL. See power.h for the why. This file owns bringing
// the PMU up (display rails, PWRON button); the cell's charge profile and
// runtime readouts live in battery.cpp, which reaches the same chip via axp().

static XPowersPMU pmu;
static bool       ok = false;

// Shared AXP2101 accessor for battery.cpp (declared in pmu.h).
XPowersPMU& axp() { return pmu; }

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

  // Cell charge profile, fuel gauge and battery/USB ADC channels. Lives in
  // battery.cpp now; must run here, after the PMU is up, for the telemetry
  // getters and the coulomb-counter to work.
  batteryConfigCharger();

  // PWRON IRQ — short / long press latched in the chip's status reg so the
  // main loop can poll powerPollButton() at any rate without losing events.
  pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
  pmu.clearIrqStatus();
  pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ |
                XPOWERS_AXP2101_PKEY_LONG_IRQ);

  // Hard power-off on a sustained PWRON hold, done in the CHIP so it works
  // even when the firmware is wedged. The old code assumed the AXP2101 did
  // this on defaults — it does not unless we set it. 6s hold → rails off.
  pmu.setPowerKeyPressOffTime(XPOWERS_POWEROFF_6S);
  pmu.setLongPressPowerOFF();

  // Hardware watchdog: the AXP2101 power-cycles the board if powerFeedWatchdog()
  // stops being called. This is the only recovery path from a firmware hang
  // that doesn't need a battery pull — the ESP32 loop-WDT can't catch a stall
  // inside a yielding FreeRTOS wait, but this chip-side timer always will.
  // 8s timeout, configured to reset the PMU and drop DCDC/LDO + PWROK so the
  // ESP32 gets a clean cold boot, not just a warm reset.
  pmu.setWatchdogTimeout(XPOWERS_AXP2101_WDT_TIMEOUT_8S);
  pmu.setWatchdogConfig(XPOWERS_AXP2101_WDT_IRQ_AND_RSET_ALL_OFF);
  pmu.clrWatchdog();      // start from a clean count
  pmu.enableWatchdog();
  return true;
}

bool powerOk() { return ok; }

void powerFeedWatchdog() {
  if (!ok) return;
  pmu.clrWatchdog();
}

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