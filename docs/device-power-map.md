# Device ⇄ power map — WS-AMOLED-1.8 (single source of truth)

Board: **Waveshare ESP32-S3-Touch-AMOLED-1.8**, PMU = **AXP2101**.

## Why this file exists

We once *assumed* `ALDO1 = 1.8 V AMOLED display rail`, set it to 1.8 V and
enabled it on boot — and that **killed audio**, because ALDO1 actually feeds
the **ES8311 codec** at 3.3 V. The guess cost real debugging time.

**Rule from now on:** never touch a rail or toggle a device's power based on a
guess. Every row below carries a **confidence** tag. Only `verified` rows may
drive code decisions. `assumed` / `unknown` rows MUST be confirmed against the
schematic (user is sourcing it) or by a measure-then-toggle test on hardware
*before* anything depends on them.

Confidence legend:

- **verified** — proven on this hardware (measured, or proven by breaking it).
- **vendor** — from the vendor's own shipping board file (Xiaozhi factory fw,
  see §2c). Authoritative for "which rails the board needs", just not measured
  by us on our build.
- **assumed** — plausible from datasheets/vendor examples, NOT proven here.
- **unknown** — no idea which rail; needs schematic.

---

## 1. Device inventory (what's on the board)

All addresses/pins verbatim from `src/board_pins.h` and the per-device HALs.

| Device         | Function                                        | Bus               | Addr / pins                                            | Driver / HAL                        |
|----------------|-------------------------------------------------|-------------------|--------------------------------------------------------|-------------------------------------|
| **ESP32-S3**   | MCU + Wi-Fi/BLE                                 | —                 | —                                                      | `main.cpp`, `ble_bridge_nimble.cpp` |
| **AXP2101**    | PMU (all rails, charger, fuel gauge, PWRON btn) | I2C (SDA15/SCL14) | `0x34`                                                 | `hal/power.cpp`                     |
| **XCA9554**    | I/O expander — holds touch/display in reset     | I2C               | `0x20`                                                 | `hal/expander.cpp`                  |
| **SH8601**     | 368×448 AMOLED, QSPI                            | QSPI              | SDIO0-3=4/5/6/7, SCLK=11, CS=12; **RST not on a GPIO** | `hal/display_amoled.cpp`            |
| **FT3168**     | capacitive touch                                | I2C               | `0x38`, INT=GPIO21                                     | `hal/touch.cpp`                     |
| **QMI8658C**   | 6-axis IMU                                      | I2C               | `0x6B`                                                 | `hal/imu.cpp`                       |
| **PCF85063A**  | RTC                                             | I2C               | `0x51`                                                 | `hal/rtc.cpp`                       |
| **ES8311**     | audio codec                                     | I2C               | `0x18` (CE low)                                        | `audio/es8311.cpp`, `hal/audio.cpp` |
| **Speaker PA** | audio power-amp enable                          | GPIO              | `AUDIO_PA_EN = GPIO46`                                 | `hal/audio.cpp`                     |
| **I2S link**   | codec audio data                                | I2S               | MCK=16, BCK=9, WS=45, DO=8, DI=10                      | `hal/audio.cpp`                     |
| **microSD**    | storage, 1-bit SDMMC                            | SDMMC             | CLK=2, CMD=1, DATA=3                                   | `hal/storage.cpp`                   |
| **BOOT btn**   | wake/back key                                   | GPIO              | `GPIO0`                                                | `main.cpp`                          |

---

## 2. AXP2101 power rails

Voltages/states below are the **measured power-on defaults**, captured
2026-06-02 by a fresh `powerDumpRails()` over serial with ALL PMU config
disabled (`#if 0` in `powerInit`) — AXP2101 chipID `0x4A`. **At reset every
rail is already ON** at a sane voltage, so the device boots fully on defaults
with zero PMU config (confirmed: `disp=1 touch=1 imu=1 rtc=1 pmu=1
audio ok=1 sound=1`). So the **voltage + on/off columns are now measured**;
only the **"Feeds" column** still carries per-row confidence.

Raw capture (ALL 14 rails — DC1-5, ALDO1-4, BLDO1-2, DLDO1-2, CPUSLDO):

```
[pmu] AXP2101 ok (chipID 0x4A)
[pmu] === power-on DEFAULTS (nothing configured yet) ===
[pmu]   DC1    3300mV ON
[pmu]   DC2     900mV ON
[pmu]   DC3    1200mV ON
[pmu]   DC4    1800mV ON
[pmu]   DC5    1400mV off
[pmu]   ALDO1  3300mV ON
[pmu]   ALDO2  3300mV ON
[pmu]   ALDO3  3000mV ON
[pmu]   ALDO4  1800mV ON
[pmu]   BLDO1  1200mV ON
[pmu]   BLDO2  2800mV ON
[pmu]   DLDO1   500mV ON
[pmu]   DLDO2   500mV ON
[pmu]   CPUSLDO 1200mV ON
[pmu] === end defaults ===
```

### Table A — what each rail is for

`Reset` = our dump (AXP factory NVM default — meaningless). `Vendor` = what the
Waveshare/Xiaozhi firmware actually sets (§2c) = what the board really needs.

| Rail    | Volt  | Reset | Vendor | Keep ON? |
|---------|-------|-------|--------|----------|
| DC1     | 3.3 V | ON    | ON     | YES      |
| ALDO1   | 3.3 V | ON    | ON     | YES      |
| DC2     | 0.9 V | ON    | OFF    | no       |
| DC3     | 1.2 V | ON    | OFF    | no       |
| DC4     | 1.8 V | ON    | OFF    | no       |
| DC5     | 1.4 V | off   | OFF    | no       |
| ALDO2   | 3.3 V | ON    | OFF    | no       |
| ALDO3   | 3.0 V | ON    | OFF    | no       |
| ALDO4   | 1.8 V | ON    | OFF    | no       |
| BLDO1   | 1.2 V | ON    | OFF    | no       |
| BLDO2   | 2.8 V | ON    | OFF    | no       |
| DLDO1   | 0.5 V | ON    | OFF    | no       |
| DLDO2   | 0.5 V | ON    | OFF    | no       |
| CPUSLDO | 1.2 V | ON    | OFF    | no       |

- **DC1** — never turn off: whole-board 3V3 (board dies without it).
- **ALDO1** — never turn off: audio codec (audio dies, no recovery).
- **All OFF rails** — unused on this board; vendor disables them. Free to turn off
  for power-saving.

### Table B — which device's power pins sit on each live rail

Both live rails are 3.3 V. The codec's analog supply is on ALDO1 (verified);
by elimination every other device pin is on DC1 (the vendor powers nothing else).
Pin names + required voltage are from each chip's datasheet (§3b).

| Rail      | Device         | Its power pin(s)            | Pin voltage spec     |
|-----------|----------------|-----------------------------|----------------------|
| **ALDO1** | ES8311 codec   | AVDD (analog)               | 3.3 V                |
| **DC1**   | ES8311 codec   | DVDD + PVDD (digital + I/O) | 1.8 or 3.3 V         |
| **DC1**   | ESP32-S3       | VDD3P3 / _RTC / _CPU / VDDA | 3.3 V                |
| **DC1**   | SH8601 display | VCI (analog) + VDDI (I/O)   | 2.7–3.6 / 1.65–3.3 V |
| **DC1**   | FT3168 touch   | AVDD + IOVCC                | 2.8–3.6 / 1.8–3.6 V  |
| **DC1**   | QMI8658 IMU    | VDD + VDDIO                 | 1.71–3.6 V           |
| **DC1**   | PCF85063 RTC   | VDD                         | 0.9–5.5 V            |
| **DC1**   | microSD        | VDD + bus I/O               | 3.3 V                |

> Caveat: the rail→device split (codec analog on ALDO1, everything else on DC1)
> is proven by the vendor enabling only those two 3.3 V rails. The exact PCB net
> for each individual pin still needs the schematic — but it must be one of these
> two 3.3 V rails, because the vendor powers nothing else.

**Bottom line: only DC1 and ALDO1 matter — both 3.3 V, both stay ON. Every other
rail is unused and the vendor turns it off.** That's the whole power map.

Notes:

- **Superseded by §2c (vendor ground truth):** the vendor enables ONLY
  **DC1 (3.3 V system)** + **ALDO1 (3.3 V codec)** and disables every other rail.
  So the "Feeds = assumed/unknown" rows below for DC2-5, ALDO2-4, BLDO1-2, DLDO1-2 mean
  **"not used by this board — safe to leave off / turn off for power-saving."**
  The datasheet candidate-matching in §3b is now mostly moot (kept for record).
  The rails showing ON in the dump above are AXP NVM defaults, not board needs.
- The display (SH8601) is **not** gated by ALDO1/ALDO3 the way we once thought —
  it works with the rails left at defaults. Which rail(s) actually feed VCI/VDDIO
  of the panel is until the schematic confirms it.
- **Resolved (2026-06-02):** the `powerSetDisplay()` landmine is gone. It
  toggled ALDO1+ALDO3 for "screen off" — cutting codec power and not actually
  blanking the panel (which isn't on those rails). Screen on/off now goes
  through the brightness path (`screenOn` → `Surface::setBrightness(0)` in
  `main.cpp`); `powerSetDisplay()` was deleted from `hal/power.{h,cpp}`.

---

## 2c. VENDOR GROUND TRUTH — Xiaozhi factory board file (authoritative)

The Waveshare factory firmware (`.tmp/ESP32-S3-Touch-AMOLED-1.8-FactoryXiaozhi_…
.bin`) is the Xiaozhi project. Its open-source board file —
`78/xiaozhi-esp32 : main/boards/waveshare/esp32-s3-touch-amoled-1.8/` — sets the
AXP2101 explicitly. This is the vendor's own bring-up for THIS board, so it's the
strongest evidence short of a schematic. The `Pmic` constructor, verbatim:

```cpp
WriteReg(0x22, 0b110); // PWRON > OFFLEVEL as POWEROFF Source enable
WriteReg(0x27, 0x10);  // hold 4s to power off
WriteReg(0x80, 0x01);  // DCDC enable reg: "Disable All DCs but DC1"  → only DC1 on
WriteReg(0x90, 0x00);  // LDO enable reg: "Disable All LDOs"
WriteReg(0x91, 0x00);  // (DLDO enable reg) → all off
WriteReg(0x82, (3300-1500)/100); // DC1   = 3.3 V   (reg 0x82 = 0x12)
WriteReg(0x92, (3300- 500)/100); // ALDO1 = 3.3 V   (reg 0x92 = 0x1C)
WriteReg(0x90, 0x01);  // enable ONLY ALDO1 — comment: "Enable ALDO1(MIC)"
WriteReg(0x64, 0x02);  // charge target voltage = 4.1 V
WriteReg(0x61, 0x02);  // precharge current = 50 mA
WriteReg(0x62, 0x08);  // charge current = 400 mA   (0x08=200,0x09=300,0x0A=400 — see note)
WriteReg(0x63, 0x01);  // termination current = 25 mA
```

### What this proves

- **The board needs only TWO rails: `DC1 = 3.3 V` (system) and `ALDO1 = 3.3 V`
  (ES8311 codec / "MIC").** The vendor explicitly **disables everything else** —
  DC2-5, ALDO2-4, BLDO1-2, DLDO1-2 — and the device ships working. So nothing
  critical hangs off those rails.
- This **doubly confirms ALDO1 → ES8311** (the "(MIC)" comment) — now vendor + verified.
- The display (SH8601), touch (FT5x06/FT3168), IMU, RTC, SD all run off the single
  **DC1 3.3 V** system rail. (Display makes its own high voltages internally;
  VCI/VDDI both accept 3.3 V.)
- **Our `powerDumpRails()` showed many rails ON only because our port doesn't
  configure the PMU** (it's `#if 0` for the defaults capture). The chip sits at
  AXP2101 NVM factory defaults (most rails on). The vendor actively turns the
  extras OFF.

### Power-saving plan (the original goal) — just copy the vendor

To minimise power: enable **DC1 + ALDO1 only**, set both to 3.3 V, disable all
other DCs/LDOs (regs `0x80=0x01`, `0x90=0x01`, `0x91=0x00`). That's exactly the
vendor's config and is the safe, proven minimal rail set. Do NOT disable DC1
(kills the whole system) or ALDO1 (kills audio).

### Note: vendor charger settings differ from our placeholders

Vendor uses **4.1 V cut-off + 400 mA** charge; our port currently has 4.2 V +
150 mA placeholders. 400 mA implies the installed cell is ≥~800 mAh (0.5C). Use
the vendor's 4.1 V / 400 mA as the reference once we re-enable charger config.

> Touch note: the vendor uses the **FT5x06** driver (`esp_lcd_touch_ft5x06`,
> addr `0x38`) for what our port calls "FT3168". Same I2C address, FT5x06-class
> protocol.

---

## 2d. Schematic-confirmed wiring (docs/ESP32-S3-Touch-AMOLED-1.8.pdf)

The board schematic (single sheet) settles the wiring. AXP2101 rail to net,
from the schematic's own power-rail table:

| Rail    | Net               | Voltage                |
|---------|-------------------|------------------------|
| DCDC1   | VCC3V3            | 3.3 V                  |
| DCDC2   | (0.9 V)           | 0.9 V                  |
| DCDC3   | (1.2 V)           | 1.2 V                  |
| DCDC4   | (1.8 V)           | 1.8 V                  |
| DCDC5   | NC                | — not connected        |
| VBAT1   | CHG_BAT           | main battery charge    |
| VBAT2   | CHG_RTC           | RTC backup-cell charge |
| ALDO1   | VL1_3.3V (= A3V3) | 3.3 V                  |
| ALDO2   | VL2_3.3V          | 3.3 V                  |
| ALDO3   | VL3_3V            | 3.0 V                  |
| ALDO4   | VL3_1.8V          | 1.8 V                  |
| BLDO1   | VL_1.2V           | 1.2 V                  |
| BLDO2   | VL_2.8V           | 2.8 V                  |
| CPUSLDO | VCL_1.2V          | 1.2 V                  |

Confirmed loads (read off the device blocks on the sheet):

- **ES8311 codec (U9):** AVDD (pin 11) <- R37 (0R) <- A3V3 <- **ALDO1**.
  DVDD + PVDD (pins 3, 4) <- R35 (0R) <- **VCC3V3 (DC1)**. So the codec's analog
  rail is ALDO1 (this is why disabling ALDO1 kills audio); its digital/IO rail is
  DC1.
- **SH8601 display (FPC U4 AXE534124):** power pins VCI (pin 24) + VDDIO (pin 26),
  enable DSI_PWR_EN (pin 22). **DSI_PWR_EN = expander EXIO1** (GPIO table). The
  panel is fed from the 3.3 V system through a switch gated by DSI_PWR_EN — NOT
  from a dedicated AXP rail. (Vendor disables every ALDO/BLDO and the panel still
  works, which independently proves this.)
- **ESP32-S3, touch FT3168, IMU, RTC, SD:** on **VCC3V3 (DC1)**.
- RTC also has a backup-cell charge rail (VBAT2 / CHG_RTC).

Rails ALDO2/3/4, BLDO1/2, CPUSLDO and DCDC2/3/4 carry net names but the **vendor
disables them and the board runs fine** -> they feed nothing the firmware needs
(spare / optional / unpopulated). DCDC5 is NC.

Live map — schematic + vendor + measurement now all agree:

- **DC1 (VCC3V3, 3.3 V)** -> ESP32-S3, display (via DSI_PWR_EN switch), touch,
  IMU, RTC, SD, and codec DVDD/PVDD.
- **ALDO1 (A3V3, 3.3 V)** -> codec AVDD.
- All other rails: off / unused.

Practical upshot: **display on/off = toggle DSI_PWR_EN (EXIO1)**, not an AXP rail.
**Power-saving = DC1 + ALDO1 on, everything else off** (vendor config, §2c).

---

## 3. Confirmed device ⇄ rail links

The only proven link so far:

| Device       | Rail      | Voltage | How proven                                                                                                                                                   |
|--------------|-----------|---------|--------------------------------------------------------------------------------------------------------------------------------------------------------------|
| ES8311 codec | **ALDO1** | 3.3 V   | verified+vendor: Setting ALDO1→1.8 V / disabling it muted the codec (commits `3672f28`→`2b34c1c`); AND vendor board file enables ALDO1 with comment "(MIC)". |
| system 3V3   | **DC1**   | 3.3 V   | Vendor enables DC1 as the only DCDC and sets it 3.3 V (§2c). Everything non-codec (display, touch, IMU, RTC, SD, ESP32-S3) runs off it.                      |

Per the vendor (§2c), display / touch / IMU / RTC / SD all sit on the **DC1
3.3 V** system rail (no dedicated rails). The exact pin-level wiring is still
unconfirmed without a schematic, but no separate rail is required for them — the
vendor disables every rail except DC1 and ALDO1.

---

## 3b. Datasheet-derived supply requirements + rail candidates

From the chip datasheets in `docs/datasheets/`. The **"needs"** column is FACT
(from each datasheet). The **"candidate rail"** column is a **voltage-match
guess only** — exactly the kind of inference that mis-identified ALDO1 once, so
NONE of these is permission to touch a rail. Confirm against the schematic or by
measure-then-toggle before acting.

| Device             | Datasheet supply pins → required V (fact)                                                                                     | Candidate AXP rail(s) by voltage match (guess)                |
|--------------------|-------------------------------------------------------------------------------------------------------------------------------|---------------------------------------------------------------|
| **ESP32-S3**       | VDD3P3 / _RTC / _CPU / VDDA all **3.3 V**; needs **no** external sub-3.3 V rail (core 1.1 V + flash 1.8 V made on-chip)       | DC1 **3.3 V** (most likely the main 3V3)                      |
| **ES8311**         | AVDD **3.3 V** (= ALDO1); DVDD **1.8 V**; PVDD **1.8 V**                                                                      | AVDD = ALDO1 verified; DVDD/PVDD → a 1.8 V rail (ALDO4 / DC4) |
| **SH8601 display** | only **2 external** rails: VDDI **~1.8 V** (I/O), VCI **~2.8 V** (analog); AVDD/VGH/VGL/ELVDD/ELVSS all generated **on-chip** | VCI 2.8 V → **BLDO2 (2.8 V)**; VDDI 1.8 V → ALDO4 / DC4       |
| **FT3168 touch**   | AVDD **2.8 V** (2.8–3.6); IOVCC **1.8 V** (or internal)                                                                       | AVDD → BLDO2 (2.8 V) or a 3.3 V rail; IOVCC → 1.8 V rail      |
| **QMI8658 IMU**    | VDD **1.8 V**, VDDIO **1.8 V** (1.71–3.6)                                                                                     | a 1.8 V rail (ALDO4 / DC4) — or 3.3 V                         |
| **PCF85063 RTC**   | VDD **~3.3 V** (0.9–5.5), ~220 nA standby                                                                                     | a 3.3 V rail (DC1 / ALDO2)                                    |
| **microSD**        | 3.3 V card + I/O                                                                                                              | a 3.3 V rail                                                  |

### Structural insights (better-grounded than pure voltage-match)

- **ESP32-S3 only needs 3.3 V externally.** It has no external DRAM and makes its
  own core/flash rails. So the AXP2101's low-voltage DC rails are NOT for the S3.
- **AXP2101's own "application suggestion" table** (datasheet Table 6-3) labels:
  DCDC1→IO/USB, **DC2→CPU**, **DC3→VSYS**, **DC4→DDR**, CPUSLDO→CPU/DDR-ref.
  Those are for an application processor with external DDR — which this board
  does NOT have. ⇒ **DC2 (0.9 V), DC3 (1.2 V), DC5 (off), DLDO1/2 (0.5 V floor),
  CPUSLDO** are most likely **vestigial AXP factory defaults left unconnected**.
  DC4 (1.8 V) is the exception — 1.8 V is genuinely useful (display VDDI, codec
  DVDD/PVDD, IMU), so DC4 *might* actually feed something. schematic needed.
- The "ON at reset" state reflects the **AXP2101's NVM factory/vendor config**,
  not the generic datasheet POR (datasheet says all rails off at POR). So a rail
  being ON ≠ a device depends on it — it may be an unconnected default.
- **Most plausible real consumers** (still assumed): 3V3 logic on DC1; codec AVDD on
  ALDO1 (verified); display VCI on BLDO2; a shared 1.8 V (DC4 or ALDO4) for display
  VDDI + codec DVDD/PVDD + IMU. To be confirmed by schematic.

### Datasheet caveats

- The AXP2101 datasheet variant on disk documents only **DCDC1-4** (no DCDC5) and
  lists DLDO1 = **DC1SW**, DLDO2 = **DC4SW**, CPUSLDO input = **DC4** — i.e. those
  are switches/sub-rails off DC1/DC4, not independent regulators. Our chip still
  reports a DC5 (1.4 V, off), so the physical part is a fuller AXP2101 variant.
- ALDO/BLDO/DLDO rails have **no** datasheet "intended use" — fully general
  purpose, so only the schematic can say what they drive here.

---

## 4. Per-device config snapshots

- **ES8311 codec registers** (healthy playback config, full decode):
  → `docs/es8311-register-dump.md`
- **AXP2101 rail dump** (`powerDumpRails()` output): captured 2026-06-02 — see
  §2 above (all 14 rails).
- Other devices: add register/config snapshots here as we capture them, same
  format (raw + plain-language), so we never have to re-reverse-engineer.

---

## 5. TODO before relying on any assumed/unknown row

1. Done — all 14 rails captured (DC1-5, ALDO1-4, BLDO1-2, DLDO1-2, CPUSLDO), §2.
2. ~~Get the board schematic~~ — largely answered by §2c (vendor board file):
   board needs only DC1 + ALDO1. Schematic still nice-to-have for exact pin nets.
3. Power-saving: implement the §2c minimal rail set (enable DC1 + ALDO1, disable
   the rest: `0x80=0x01`, `0x90=0x01`, `0x91=0x00`). Re-enable charger at the
   vendor's 4.1 V / 400 mA. This replaces the `#if 0` defaults-capture block.
4. Done — the `powerSetDisplay()` landmine is removed; screen-off now uses
   `Surface::setBrightness(0)` via `screenOn`, so it no longer cuts the codec rail.

Datasheets for cross-checking: `docs/datasheets/` (AXP2101, ES8311, SH8601A0,
FT3168, QMI8658C, PCF85063A, ESP32-S3).