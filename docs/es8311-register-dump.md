# ES8311 register dump (after `es8311_init`)

Captured over serial on the WS-AMOLED-1.8 unit on 2026-06-02, right after the
codec driver finished its init (the log line `[audio] ES8311 ok` follows it).
Source of the dump: `es8311_register_dump()` in `src/audio/es8311.cpp:428`
(loops regs `0x00`–`0x49`, prints `REG:xx: vv` with no newline, so the raw log
runs all values together on one line).

> NOTE: this is the **ES8311 audio codec** state, NOT the AXP2101 power rails.
> The AXP2101 rail dump (`[pmu] DC1/ALDO1/…`) is a separate thing — see
> `src/hal/power.cpp::powerDumpRails()`.

Audio was confirmed working in this same boot (`[3i.4] audio ok=1 sound=1`),
so this dump is a known-good reference of a healthy codec configuration.

---

## Raw

Only `0x00`–`0x3f` were captured in this paste (the trailing `0x40`–`0x49`
GPIO/test regs scrolled off):

```
REG:00: 80  REG:01: 3f  REG:02: 48  REG:03: 10
REG:04: 10  REG:05: 00  REG:06: 03  REG:07: 00
REG:08: ff  REG:09: 0c  REG:0a: 0c  REG:0b: 00
REG:0c: 20  REG:0d: 01  REG:0e: 02  REG:0f: 00
REG:10: 13  REG:11: 7c  REG:12: 00  REG:13: 10
REG:14: 1a  REG:15: 00  REG:16: 04  REG:17: c8
REG:18: 00  REG:19: 00  REG:1a: 00  REG:1b: 0c
REG:1c: 6a  REG:1d: 00  REG:1e: 00  REG:1f: 00
REG:20: 00  REG:21: 00  REG:22: 00  REG:23: 00
REG:24: 00  REG:25: 00  REG:26: 00  REG:27: 00
REG:28: 00  REG:29: 00  REG:2a: 00  REG:2b: 00
REG:2c: 00  REG:2d: 00  REG:2e: 00  REG:2f: 00
REG:30: 00  REG:31: 00  REG:32: ff  REG:33: 00
REG:34: 00  REG:35: 00  REG:36: 00  REG:37: 08
REG:38: 00  REG:39: 00  REG:3a: 00  REG:3b: 00
REG:3c: 00  REG:3d: 00  REG:3e: 00  REG:3f: 00
```

Original single-line paste, verbatim:

```
REG:00: 80REG:01: 3fREG:02: 48REG:03: 10REG:04: 10REG:05: 00REG:06: 03REG:07: 00REG:08: ffREG:09: 0cREG:0a: 0cREG:0b: 00REG:0c: 20REG:0d: 01REG:0e: 02REG:0f: 00REG:10: 13REG:11: 7cREG:12: 00REG:13: 10REG:14: 1aREG:15: 00REG:16: 04REG:17: c8REG:18: 00REG:19: 00REG:1a: 00REG:1b: 0cREG:1c: 6aREG:1d: 00REG:1e: 00REG:1f: 00REG:20: 00REG:21: 00REG:22: 00REG:23: 00REG:24: 00REG:25: 00REG:26: 00REG:27: 00REG:28: 00REG:29: 00REG:2a: 00REG:2b: 00REG:2c: 00REG:2d: 00REG:2e: 00REG:2f: 00REG:30: 00REG:31: 00REG:32: ffREG:33: 00REG:34: 00REG:35: 00REG:36: 00REG:37: 08REG:38: 00REG:39: 00REG:3a: 00REG:3b: 00REG:3c: 00REG:3d: 00REG:3e: 00REG:3f: 00
```

---

## In plain language

Register names from `src/audio/es8311_reg.h`; values cross-checked against what
the driver actually writes in `src/audio/es8311.cpp`. Datasheet:
`docs/datasheets/ES8311.DS.pdf` (+ `ES8311.user.Guide.pdf`).

| Reg           | Name                       | Val           | Meaning                                                                                                              |
|---------------|----------------------------|---------------|----------------------------------------------------------------------------------------------------------------------|
| `0x00`        | RESET / CSM / clock-mgr    | `0x80`        | **Powered on, out of reset.** `0x80` is the driver's final "power-on command" (after the `0x1F`→`0x00` reset pulse). |
| `0x01`        | CLK_MANAGER                | `0x3f`        | **Codec clocks running.** MCLK source + the bank of internal clock-enable bits all on.                               |
| `0x02`        | CLK_MANAGER (div/mult)     | `0x48`        | Clock divider / multiplier coefficients for the current MCLK↔sample-rate ratio.                                      |
| `0x03`        | ADC FSMODE / OSR           | `0x10`        | ADC oversample-rate / FS-mode coefficient (set per the clock table).                                                 |
| `0x04`        | DAC OSR                    | `0x10`        | DAC oversample ratio.                                                                                                |
| `0x05`        | ADC+DAC clk divider        | `0x00`        | No extra division (÷1).                                                                                              |
| `0x06`        | BCLK inverter / divider    | `0x03`        | BCLK divider; not inverted.                                                                                          |
| `0x07`        | tri-state / LRCK div hi    | `0x00`        | Outputs not tri-stated.                                                                                              |
| `0x08`        | LRCK div lo                | `0xff`        | LRCK divider low byte (frame length coefficient).                                                                    |
| `0x09`        | **SDP IN** (DAC path fmt)  | `0x0c`        | **I2S, 16-bit, slave.** `0x0c = (3<<2)` is exactly the driver's 16-bit code (`es8311_resolution_config`).            |
| `0x0a`        | **SDP OUT** (ADC path fmt) | `0x0c`        | **I2S, 16-bit, slave.** Same as in.                                                                                  |
| `0x0b`        | SYSTEM                     | `0x00`        | Default.                                                                                                             |
| `0x0c`        | SYSTEM                     | `0x20`        | Default.                                                                                                             |
| `0x0d`        | SYSTEM power up/down       | `0x01`        | **Analog circuitry powered up** (driver writes `0x01`, "NOT default").                                               |
| `0x0e`        | SYSTEM power up/down       | `0x02`        | **Analog PGA + ADC modulator enabled** (driver `0x02`, "NOT default").                                               |
| `0x0f`        | SYSTEM low-power           | `0x00`        | Low-power modes off (full performance).                                                                              |
| `0x10`        | SYSTEM                     | `0x13`        | VMID / charge-pump / bias setup.                                                                                     |
| `0x11`        | SYSTEM                     | `0x7c`        | Bias / reference setup.                                                                                              |
| `0x12`        | SYSTEM (enable DAC)        | `0x00`        | **DAC powered up** (driver writes `0x00` to power it on, "NOT default").                                             |
| `0x13`        | SYSTEM                     | `0x10`        | **Output to HP/line drive enabled** (driver `0x10`, "NOT default").                                                  |
| `0x14`        | SYSTEM (DMIC / PGA gain)   | `0x1a`        | **Analog MIC selected, max analog PGA gain** (driver `0x1A`).                                                        |
| `0x15`        | ADC ramp / DMIC sense      | `0x00`        | Default ramp.                                                                                                        |
| `0x16`        | ADC                        | `0x04`        | ADC gain-scale setting.                                                                                              |
| `0x17`        | ADC volume                 | `0xc8`        | **ADC digital volume `0xC8` (200)** — the gain the driver hard-codes at init.                                        |
| `0x18`–`0x1b` | ADC ALC / HPF              | `00 00 00 0c` | ALC essentially off; `0x1b=0x0c` = ADC high-pass-filter stage-1 setting.                                             |
| `0x1c`        | ADC EQ / HPF s2            | `0x6a`        | **ADC equalizer bypass + digital DC-offset cancel** (driver `0x6A`).                                                 |
| `0x1d`–`0x31` | ADC ALC / DAC mute         | all `0x00`    | ALC/DRC unused; `0x31=0x00` ⇒ **DAC not muted**.                                                                     |
| `0x32`        | **DAC volume**             | `0xff`        | **DAC digital volume = `0xFF` → 0 dB (max / full scale).**                                                           |
| `0x33`        | DAC offset                 | `0x00`        | No offset.                                                                                                           |
| `0x34`–`0x36` | DAC DRC                    | `0x00`        | Dynamic-range compression off.                                                                                       |
| `0x37`        | DAC ramprate               | `0x08`        | **DAC equalizer bypassed** (driver `0x08`, "NOT default").                                                           |
| `0x38`–`0x3f` | reserved / GPIO area       | `0x00`        | Defaults.                                                                                                            |

### One-paragraph summary

The codec is **fully powered up and configured for playback**: out of reset
(`0x00=0x80`), clocks running (`0x01=0x3f`), digital interface is **I2S /
16-bit / slave** on both directions (`0x09=0x0a=0x0c`), the **DAC is on and
unmuted at 0 dB** (`0x12=0x00`, `0x31=0x00`, `0x32=0xff`) with the output
driver enabled (`0x13=0x10`), and the analog front end (PGA/ADC, analog mic at
max gain) is up (`0x0d=0x01`, `0x0e=0x02`, `0x14=0x1a`, `0x17=0xc8`). EQ/DRC/ALC
are all bypassed. Every "NOT default" value the driver sets is present in the
dump, so `es8311_init` ran end-to-end — consistent with `audio ok=1 sound=1`.

### Open follow-ups

- Capture the missing tail `0x40`–`0x49` (GPIO `0x44`/`0x45` etc.) for a
  complete picture — increase the serial buffer or add a `\n` per line in
  `es8311_register_dump()` so the paste doesn't run together.
- This is the **codec** dump. Still pending: the **AXP2101 power-on rail
  defaults** (`[pmu]` lines from `powerDumpRails()`), which is the actual
  power-investigation goal.
