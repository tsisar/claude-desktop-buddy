# Port: Waveshare ESP32-S3-Touch-AMOLED-1.8

Porting the buddy firmware from the **M5StickC Plus** (ESP32, 135×240 ST7789
via TFT_eSPI, AXP192 PMU, MPU6886 IMU, two physical buttons) to the
**Waveshare ESP32-S3-Touch-AMOLED-1.8** (ESP32-S3R8, 368×448 SH8601 AMOLED
over QSPI, capacitive touch, AXP2101 PMU, QMI8658 IMU, no physical A/B
buttons).

Branch: `port/ws-amoled-18`. The original M5 build is preserved unchanged
under `[env:m5stickc-plus]`; the new board builds under `[env:ws-amoled-18]`.

## Target hardware (verified facts)

Source of truth for pins: the official Waveshare engineering-sample repo
`waveshareteam/ESP32-S3-Touch-AMOLED-1.8`,
`examples/Arduino-v3.3.5/libraries/Mylibrary/pin_config.h`. (An early forum
hit listed CS=9/CLK=10 — that is **wrong** for this board; ignore it.)

| Block | Part | Bus | Pins / address |
| --- | --- | --- | --- |
| Display | SH8601, 368×448, 16.7M color | QSPI | CS=12, SCLK=11, D0=4, D1=5, D2=6, D3=7; RST not wired (`GFX_NOT_DEFINED`), no EN toggle — just `gfx->begin()` |
| Touch | FT3168 (FT6x36-class) | I²C | SDA=15, SCL=14, INT=21, addr `0x38` |
| IMU | QMI8658 (6-axis) | I²C | shared SDA=15/SCL=14, addr `0x6B` |
| RTC | PCF85063 | I²C | shared bus, addr `0x51` |
| PMU | AXP2101 (charge/battery) | I²C | shared bus, addr `0x34` (`XPOWERS_CHIP_AXP2101`) |
| Audio | ES8311 codec + speaker | I²S | MCLK=16, BCLK=9, WS=45, DO=8, DI=10, PA_EN=46 |
| microSD | SDMMC 1-bit | — | CLK=2, CMD=1, DAT=3 |
| Buttons | PWR (via AXP2101 IRQ), BOOT (GPIO0) | — | no A/B buttons — UI must move to **touch** |

Confirmed straight from `pin_config.h`: LCD_SDIO0..3 = 4/5/6/7, LCD_SCLK=11,
LCD_CS=12, LCD_WIDTH=368, LCD_HEIGHT=448, IIC_SDA=15, IIC_SCL=14, TP_INT=21.
The HelloWorld example constructs the panel as `Arduino_ESP32QSPI(CS,SCLK,
D0,D1,D2,D3)` → `Arduino_SH8601(bus, GFX_NOT_DEFINED, 0, 368, 448)` and just
calls `gfx->begin()` + `gfx->setBrightness(255)`; no reset/EN GPIO toggling.

SoC: ESP32-S3R8, dual LX7 @240MHz, 512KB SRAM, **8MB OPI PSRAM**, **16MB
QIO flash**, Wi-Fi + BLE5. PlatformIO: `board = esp32-s3-devkitc-1`,
`memory_type = qio_opi`, `flash_size = 16MB`, `-DBOARD_HAS_PSRAM`.

The 368×448×16bpp full-screen canvas is ~322KB — lives in PSRAM, not in the
512KB internal SRAM.

## Library mapping

| Concern | M5 (old) | AMOLED (new) |
| --- | --- | --- |
| Display + offscreen canvas | `M5StickCPlus` / `TFT_eSprite` | `GFX Library for Arduino` (moononournation): `Arduino_ESP32QSPI` + `Arduino_SH8601` + `Arduino_Canvas` |
| GIF decode | `bitbank2/AnimatedGIF` | same |
| JSON | `bblanchon/ArduinoJson` | same |
| IMU | `M5.Imu` (MPU6886) | `lewisxhe/SensorLib` → `SensorQMI8658` |
| PMU / battery | `M5.Axp` (AXP192) | `lewisxhe/XPowersLib` → `XPowersAXP2101` |
| RTC | `M5.Rtc` | `SensorLib` `SensorPCF85063` (or AXP2101) |
| Touch | — | FT3168 over I²C (direct, or `Arduino_DriveBus` FT3x68) |
| Buzzer | `M5.Beep` (passive buzzer) | no buzzer — board has an **ES8311 codec + speaker** instead; `beep()` starts as a no-op, optional later: short I²S tone via the codec |

Official Waveshare Arduino examples themselves use Arduino_GFX
(`Arduino_ESP32QSPI` + `Arduino_SH8601` + `Arduino_Canvas`) and ship an LVGL
sample too — confirming Arduino_GFX is the native path for this panel.

## Graphics strategy

Decision: **Arduino_GFX `Arduino_Canvas`** as a drop-in for `TFT_eSprite`
(not LVGL). The entire current UI is immediate-mode (`spr.fillRect`,
`spr.setCursor`, `spr.print`, `spr.pushSprite`) and the 18 species + GIF
pipeline draw straight to the sprite; Canvas maps onto that 1:1
(`flush()` == `pushSprite()`), so ~90% of drawing code and the whole GIF
path survive. A TFT_eSPI-compat `Surface` wrapper (see `src/hal/display.h`)
provides the method names the existing code already calls, including
`setTextDatum`/`drawString` centering, which Arduino_GFX lacks natively.

LVGL can be layered on later (optional stage 6) if a touch-native widget UI
is wanted; the canvas stays as the pet/GIF surface.

## Input strategy (no A/B/Power buttons)

The M5 build maps everything to BtnA (next screen / approve), BtnB (page /
deny), hold-A (menu), AXP power button (screen off). On AMOLED there are no
such buttons, so input moves to the touchscreen:

- **Approval screen:** large on-screen **Approve** / **Deny** tap targets.
- **Navigation:** tap-zones (left/right thirds = prev/next screen), a
  hamburger/long-press-equivalent (top-corner tap) opens the menu.
- **Menus:** tap a row to select, tap again / dedicated button to confirm.
- BOOT (GPIO0) kept as an emergency "wake / back" hardware button.

This replaces the `M5.BtnA/BtnB/Axp.GetBtnPress` logic in `loop()`.

## Staged plan

- **Stage 0 — scaffold.** Branch, `[env:ws-amoled-18]`, `board_pins.h`,
  per-env `build_src_filter` so the new env compiles independently. ✅
- **Stage 1 — HAL boundary.** `src/hal/display.h` (Surface API == the subset
  of TFT_eSPI the code uses) + input/IMU/power/RTC HAL headers. ✅ (display)
- **Stage 2 — display bring-up.** `bringup_amoled.cpp`: init panel, draw
  "hello buddy", read touch + IMU, print to USB serial. Prove the hardware.
  ✅ **Verified on device** — serial shows `display OK (psram=8388608
  free=8385908)`, `QMI8658 OK`, `FT3168 OK at 0x38`, live accel
  `a(0.06,0.05,-0.98)` (z≈−1g, board face-up). Built with pioarduino +
  octal PSRAM, flashed over USB-JTAG.
- **Stage 3 — port `main.cpp`.** Replace `M5.*`/`spr` with HAL: Surface for
  draw, QMI8658 for shake/face-down/orientation, AXP2101 for battery/power,
  PCF85063 for clock, touch for input. Rescale geometry 135×240 → 368×448.
- **Stage 4 — species + GIF.** Species files draw via Surface (drop
  `#include <M5StickCPlus.h>`, `extern Surface spr`). Re-center the 18 ASCII
  pets and the GIF canvas for the larger screen; bump GIF target size.
- **Stage 5 — BLE + xfer.** `ble_bridge`/`data`/`xfer` are already
  hardware-agnostic (only `M5.Axp` reads in `xfer.h` status need swapping for
  AXP2101). Wire protocol and `characters/` packs are unchanged.
- **Stage 6 (optional) — LVGL** touch-native UI shell.

## Notes / gotchas

- Geometry constants `W=135,H=240` and the hardcoded layout coords (panels,
  HUD area, clock positions, `BUDDY_X_CENTER=67`, `BUDDY_Y_BASE=30`) are
  tuned for 135×240. The larger 368×448 screen needs a scale pass — either
  bump the base text size or introduce a layout scale factor.
- `setBrightness()` is on the SH8601 panel object, not the canvas.
- No panel reset/EN GPIO — `gfx->begin()` is enough (matches HelloWorld).
- No buzzer: `beep()` is a no-op; optional tone later via ES8311 codec.
- Touch/IMU/RTC/PMU all share **one** I²C bus (SDA=15, SCL=14). Init it once
  and hand the same `Wire` to every driver.
- **Toolchain:** the stock PlatformIO `espressif32` platform ships
  arduino-esp32 **2.0.x**, which lacks `esp32-hal-periman.h` that
  Arduino_GFX 1.4.x needs → build fails. Use the **pioarduino** platform
  (core 3.x) instead — see the `platform = …pioarduino…` line in
  `platformio.ini`.
- **Octal PSRAM:** the board is ESP32-S3R8 with *octal* PSRAM, but
  `esp32-s3-devkitc-1` defaults to *quad* (`qio_qspi`). Without
  `board_build.arduino.memory_type = qio_opi` the boot log shows
  `quad_psram: chip not connected` and the 322KB canvas alloc fails →
  `display begin() FAILED` → crash. With it: `psram=8388608` and all good.
- **USB is native USB-Serial/JTAG** (not an external UART). `Serial` needs
  `-DARDUINO_USB_CDC_ON_BOOT=1` (set) to appear on the port, and
  `Serial.setTxTimeoutMs(0)` so prints don't block when no monitor is open.
- Partition: start with `default_16MB.csv`; a custom no-OTA layout gives more
  LittleFS room for GIF character packs if needed.
- Cannot build/flash from this environment — all new code is **pending
  on-device verification** with `pio run -e ws-amoled-18 -t upload`.
