# Port: Waveshare ESP32-S3-Touch-AMOLED-1.8

Porting the buddy firmware from the **M5StickC Plus** (ESP32, 135×240 ST7789
via TFT_eSPI, AXP192 PMU, MPU6886 IMU, two physical buttons) to the
**Waveshare ESP32-S3-Touch-AMOLED-1.8** (ESP32-S3R8, 368×448 SH8601 AMOLED
over QSPI, capacitive touch, AXP2101 PMU, QMI8658 IMU, no physical A/B
buttons).

Branch: `port/ws-amoled-18`. The board builds under `[env:ws-amoled-18]`,
which is now the **only** environment — the original `[env:m5stickc-plus]`
section and the M5-specific sources have since been removed from
`platformio.ini` (all of `src/` is live AMOLED code).

## Target hardware (verified facts)

Source of truth for pins: the official Waveshare engineering-sample repo
`waveshareteam/ESP32-S3-Touch-AMOLED-1.8`,
`examples/Arduino-v3.3.5/libraries/Mylibrary/pin_config.h`. (An early forum
hit listed CS=9/CLK=10 — that is **wrong** for this board; ignore it.)

| Block   | Part                                | Bus  | Pins / address                                                                                                |
|---------|-------------------------------------|------|---------------------------------------------------------------------------------------------------------------|
| Display | SH8601, 368×448, 16.7M color        | QSPI | CS=12, SCLK=11, D0=4, D1=5, D2=6, D3=7; RST not wired (`GFX_NOT_DEFINED`), no EN toggle — just `gfx->begin()` |
| Touch   | FT3168 (FT6x36-class)               | I²C  | SDA=15, SCL=14, INT=21, addr `0x38`                                                                           |
| IMU     | QMI8658 (6-axis)                    | I²C  | shared SDA=15/SCL=14, addr `0x6B`                                                                             |
| RTC     | PCF85063                            | I²C  | shared bus, addr `0x51`                                                                                       |
| PMU     | AXP2101 (charge/battery)            | I²C  | shared bus, addr `0x34` (`XPOWERS_CHIP_AXP2101`)                                                              |
| Audio   | ES8311 codec + speaker              | I²S  | MCLK=16, BCLK=9, WS=45, DO=8, DI=10, PA_EN=46                                                                 |
| microSD | SDMMC 1-bit                         | —    | CLK=2, CMD=1, DAT=3                                                                                           |
| Buttons | PWR (via AXP2101 IRQ), BOOT (GPIO0) | —    | no A/B buttons — UI must move to **touch**                                                                    |

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

| Concern                    | M5 (old)                       | AMOLED (new)                                                                                                                          |
|----------------------------|--------------------------------|---------------------------------------------------------------------------------------------------------------------------------------|
| Display + offscreen canvas | `M5StickCPlus` / `TFT_eSprite` | `GFX Library for Arduino` (moononournation): `Arduino_ESP32QSPI` + `Arduino_SH8601` + `Arduino_Canvas`                                |
| GIF decode                 | `bitbank2/AnimatedGIF`         | same                                                                                                                                  |
| JSON                       | `bblanchon/ArduinoJson`        | same                                                                                                                                  |
| IMU                        | `M5.Imu` (MPU6886)             | `lewisxhe/SensorLib` → `SensorQMI8658`                                                                                                |
| PMU / battery              | `M5.Axp` (AXP192)              | `lewisxhe/XPowersLib` → `XPowersAXP2101`                                                                                              |
| RTC                        | `M5.Rtc`                       | `SensorLib` `SensorPCF85063` (or AXP2101)                                                                                             |
| Touch                      | —                              | Direct FT3168 driver in `src/hal/touch.cpp` (5-byte register read at 0x02). Gesture layer on top in `src/gesture.cpp` (tap / swipe) |
| GPIO expander              | —                              | XCA9554 at I²C `0x20` — pulses RESET on touch/display/etc at boot via `src/hal/expander.cpp` (called from `powerInit`)         |
| Buzzer                     | `M5.Beep` (passive buzzer)     | No buzzer — board uses **ES8311 codec + speaker**. `src/hal/audio.cpp` + `src/audio/es8311.cpp` provide non-blocking `audioBeep` / `audioClick` / `audioClickSoft` (a FreeRTOS task synthesizes the PCM); verified on hardware |

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

> **Historical plan — superseded.** This section is the original porting
> plan; several details drifted during implementation (the menu opens on a
> BOOT tap, swipe up/down cycle the home views, and BOOT became the primary
> UI button). The shipped behaviour is documented in `docs/USER-GUIDE.md` —
> trust that over this section.

The M5 build maps everything to BtnA (next screen / approve), BtnB (page /
deny), hold-A (menu), AXP power button (screen off). On AMOLED there are no
A/B buttons, so input moves to touch gestures + the PWRON button. Full
mapping in `.tmp/buttons-map.md`; summary here:

- **Approval screen:** **swipe right = APPROVE**, **swipe left = DENY**
  (no on-screen buttons — the whole canvas is the target).
- **Menu (settings / reset / main):** opens on a **BOOT tap** (shipped;
  the plan's swipe-up open was dropped); tap row = select+confirm in one
  (no cycle); tap outside / `back` button = close.
- **Cycle display mode** (NORMAL ↔ PET ↔ INFO): **swipe down** (forward)
  / **swipe up** (back).
- **INFO / PET pagination**: swipe left / right.
- **Reset confirmation**: modal dialog with `Confirm` / `Cancel` (replaces
  the M5 tap-twice arm/fire).
- **Transcript scroll-back**: tap the HUD zone → full-screen scrollable
  transcript view with `back`.
- **Power button (AXP2101 PWRON)**: short tap = screen off/on, hold ≥6s =
  hard power-off (chip-level, same as M5).
- **BOOT (GPIO0)** — originally pencilled in as reserved for the ES8311
  audio path; it shipped as the **primary UI button** instead (tap =
  open menu / activate row, hold = screenshot — see the user guide).
- **Shake (QMI8658)** → DIZZY one-shot, same algorithm as M5
  (delta > 0.8g, polled every 20ms).
- **Face-down (QMI8658)** → nap mode (same debounce: enter ≥15 frames,
  exit ≤−8 frames, az < −0.7g).

Gesture recognizer lives in `src/gesture.cpp`. State machine: pen-down →
record start (x0, y0, t0) → pen-up → classify by displacement + elapsed:
TAP (<15px, <300ms), SWIPE (≥80px along dominant axis). Decisions happen
on release — no live-fire during drag. See `src/gesture.h` for tunables.

This replaces the `M5.BtnA/BtnB/Axp.GetBtnPress` logic in `loop()`.

## Staged plan

- **Stage 0 — scaffold.** Branch, `[env:ws-amoled-18]`, `board_pins.h`,
  per-env `build_src_filter` so the new env compiles independently. ✅
- **Stage 1 — HAL boundary.** `src/hal/display.h` (Surface API == the subset
  of TFT_eSPI the code uses) + input/IMU/power/RTC HAL headers. ✅ (display)
- **Stage 2 — display bring-up.** `bringup_amoled.cpp`: init panel, draw
  "hello buddy", read touch + IMU, print to USB serial. Prove the hardware.
  ✅ **Verified on device.** Before the PSRAM fix every boot logged
  `quad_psram: chip not connected` → `display begin() FAILED` →
  StoreProhibited crash-loop. After it, the board runs `loop()` steadily
  with `imu=1 a(0.06,0.05,-0.98)` (z≈−1g, face-up) and **no crash** — which
  only happens once PSRAM is up and the 322KB canvas allocated. Built with
  pioarduino + octal PSRAM, flashed over native USB-Serial/JTAG.
  (The one-shot boot banner with the exact PSRAM size scrolls past before
  the USB-JTAG port re-enumerates; the steady crash-free loop is the proof.)
- **Stage 3 — port `main.cpp`.** Replace `M5.*`/`spr` with HAL. Split into
  sub-stages, each verified on hardware before merging:
  - **3a/3b/3c — species through Surface.** ✅ Layout proof → animate cat
    (7 states) → all 18 species compile through the new Surface API.
  - **3d — BLE NimBLE bridge.** ✅ Stock BLE-Arduino conflicts with the
    pioarduino core; switched to NimBLE. Claude desktop connects.
  - **3f — buddy alive over BLE.** ✅ `src/stage3f_amoled.cpp`: minimal
    runtime with BLE-heartbeat-driven persona state, on-screen APPROVE/
    DENY touch zones, audio beep on prompt arrival.
  - **3g — sensor HAL verification.** ✅ `src/stage3g_amoled.cpp`:
    standalone harness for QMI8658 (accel + shake + face-down + temp),
    PCF85063 (time/date), and AXP2101 PWRON (short/long press IRQ).
    Closed the M5-side hardware features stage 3f couldn't reach.
  - **3h — touch + gesture verification.** ✅ `src/stage3h_amoled.cpp` +
    `src/gesture.cpp`: tap / swipe up/down/left/right recognition on top
    of the touch HAL. Verified end-to-end on hardware.
  - **3i — fold HAL into a unified UI port** in `src/stage3j_amoled.cpp`.
    Built up in five verified sub-stages:
    - **3i.1 — backend.** ✅ `src/data_amoled.h` + `src/stats.h`. Full
      heartbeat parser, RTC sync via `hal/rtc.h`, transcript / owner /
      petname through NVS. `stats.h` (mood / fed / energy / level + NVS)
      stays mostly the M5 version with one std::min cast fix for
      arduino-esp32 core 3.x.
    - **3i.2 — UI shell.** ✅ Home (CAT-only buddy + transcript HUD) +
      full approval screen + tap-to-scroll HUD. Approve / Deny on swipe
      right / left. UTF-8 → '?' strip (later replaced by a real font in
      the cyrillic font commit).
    - **3i.3 — menu + settings + reset modal.** ✅ Modal stack: main /
      settings / reset / confirm. Touch (tap row) AND physical buttons
      share the same `navIdx`. (The 3i.3 button roles were later
      simplified to today's map: BOOT tap = select, PWRON tap = cursor
      down, BOOT hold = screenshot — see `docs/USER-GUIDE.md`.) Reset
      confirm replaces M5's tap-twice arm pattern. NVS persists every
      settings toggle.
    - **3i.4 — INFO + PET + clock face + DISP cycling.** ✅
      DISP_NORMAL / PET / INFO cycled with swipe-down. 2 PET pages
      (stats / how-to), 6 INFO pages (ABOUT / CONTROLS / CLAUDE /
      DEVICE / BLUETOOTH / CREDITS). Clock face takes over the home
      view when parked on USB + RTC synced + no live work.
    - **3i.5 — auto-behaviours.** ✅ Shake → DIZZY one-shot (QMI8658
      magnitude delta), face-down nap with stats accumulation, 30s
      auto screen-off on battery, 12s SLEEP-hold after waking, full-
      screen passkey display during BLE pairing.
- **Stage 4 — species + GIF custom characters.** Built up in four
  sub-stages, fully wired by 4d (end-to-end push verification still
  pending Claude Desktop):
  - **4a — species registry + cycling.** ✅ `src/buddy_amoled.cpp`
    replaces M5's `buddy.cpp` without the TFT_eSprite / M5 dependency.
    All 18 species cycle through Settings → "ascii pet"; NVS persists
    the choice.
  - **4b — hybrid storage HAL.** ✅ `src/hal/storage.h` +
    `storage_amoled.cpp`. SDMMC (1-bit) first, LittleFS fallback,
    STORAGE_NONE if both fail. Callers get a single `fs::FS&` so the
    xfer path stays backend-agnostic.
  - **4c — xfer.h folder-push transport.** Compiles. `src/xfer_amoled.h`
    speaks the same wire protocol as Claude Desktop (cmd:char_begin /
    file / chunk / file_end / char_end) plus housekeeping cmd:status /
    name / owner / species / unpair. `tools/usb_xfer_send.{py,sh}`
    pushes a folder over USB-CDC for local verification.
  - **4d — character.cpp GIF playback.** Compiles. `src/character_amoled.cpp`
    reads `/characters/<name>/manifest.json`, opens GIFs through
    AnimatedGIF, renders scanlines through `Surface::drawPixel`. xfer's
    `char_end` triggers `characterInit(name)` so a fresh push becomes the
    active buddy immediately. Tri-state Settings → "ascii pet" cycles
    ASCII 0..N-1 → GIF (when installed) → 0.
- **Cyrillic font.** ✅ `src/fonts/u8g2_font_6x12_t_cyrillic.{cpp,h}` —
  191-glyph public-domain u8g2 font with full RU+UA coverage. The data
  path keeps UTF-8 verbatim; `Surface::setUTF8Print(true)` and
  `setFont(u8g2_font_6x12_t_cyrillic)` in `setup()` make `Привіт` render
  as actual Cyrillic instead of `?`. Build flag `-DU8G2_FONT_SUPPORT`
  unlocks the u8g2 path inside Arduino_GFX.
- **Stage 5 — final BLE/xfer integration.** End-to-end folder push +
  GIF render verification with Claude Desktop on macOS/Windows. Code
  side is done; this is purely the on-hw confirmation.
- **Stage 6 (optional) — LVGL** touch-native UI shell.

## Notes / gotchas

- Geometry constants `W=135,H=240` and the hardcoded layout coords (panels,
  HUD area, clock positions, `BUDDY_X_CENTER=67`, `BUDDY_Y_BASE=30`) are
  tuned for 135×240. The larger 368×448 screen needs a scale pass — either
  bump the base text size or introduce a layout scale factor.
- `setBrightness()` is on the SH8601 panel object, not the canvas.
- No panel reset/EN GPIO — `gfx->begin()` is enough (matches HelloWorld).
- Speaker: ES8311 + I²S clicks/beeps, verified — see `src/hal/audio.cpp`
  (non-blocking; a FreeRTOS task synthesizes the tones).
- Touch/IMU/RTC/PMU/expander all share **one** I²C bus (SDA=15, SCL=14).
  Init it once and hand the same `Wire` to every driver.
- **XCA9554 GPIO expander at 0x20 holds touch/display RESET lines.** On a
  cold cycle (e.g. when `powerInit` toggles the AXP2101 ALDO rails), the
  FT3168 misses its first I²C ACK unless these RESET lines get a low→high
  pulse via the expander. `powerInit()` does this automatically — repeats
  the verbatim sequence from the vendor `04_GFX_FT3168_Image` example.
  Without it, stage 3h's clean power cycle locks touch out, even though
  stage 3f (which never toggled the rails) "worked" by accident.
- **QMI8658 axis quirks (verified on hardware):**
  - Returns g-units already (raw int16 × `range/32768`), NOT m/s² — do
    NOT divide by 9.80665 a second time.
  - z+ points INTO the screen on this board → board-flat z reads ≈ −1g.
    `imu_amoled.cpp` flips z in HAL so the M5 face-down threshold
    (`az < −0.7`) ports unchanged.
- **PCF85063** address is hardcoded `0x51` inside `SensorPCF85063` —
  matches this board.
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
- Build / flash: `make build` / `make flash` / `make flash-monitor`. PIO
  binary auto-discovered at `~/.platformio/penv/bin/pio` (Makefile falls
  back to PATH). Serial port default picks `/dev/ttyACM0` on Linux,
  `/dev/cu.usbmodem11401` on macOS; override with `make flash PORT=...`.
