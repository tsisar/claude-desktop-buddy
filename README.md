# claude-desktop-buddy

Claude for macOS and Windows can connect Claude Cowork and Claude Code to
maker devices over BLE, so developers and makers can build hardware that
displays permission prompts, recent messages, and other interactions. We've
been impressed by the creativity of the maker community around Claude -
providing a lightweight, opt-in API is our way of making it easier to build
fun little hardware devices that integrate with Claude.

> **Building your own device?** You don't need any of the code here. See
> **[REFERENCE.md](REFERENCE.md)** for the wire protocol: Nordic UART
> Service UUIDs, JSON schemas, and the folder push transport.

As an example, we built a desk pet on ESP32 that lives off permission
approvals and interaction with Claude. It sleeps when nothing's happening,
wakes when sessions start, gets visibly impatient when an approval prompt is
waiting, and lets you approve or deny right from the device.

<p align="center">
  <img src="docs/device.jpg" alt="Waveshare ESP32-S3-Touch-AMOLED-1.8 running the buddy firmware" width="500">
</p>

## Hardware

The firmware targets the **Waveshare ESP32-S3-Touch-AMOLED-1.8**
(ESP32-S3R8, 16MB flash, 8MB PSRAM) with the Arduino framework: a 368×448
AMOLED (SH8601 over QSPI) driven through moononournation's **GFX Library
for Arduino**, FT3168 capacitive touch, QMI8658 IMU, AXP2101 PMU, PCF85063
RTC, ES8311 audio codec + speaker, and a microSD slot. The desktop bridge
talks to it over BLE or USB-CDC. Library deps are pinned in
`platformio.ini`; the board-specific drivers live under `src/hal/`, so a
different board means swapping that layer for your own pin layout.

## Flashing

Install
[PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/),
then:

```bash
make flash          # or: pio run -e ws-amoled-18 -t upload
```

(`make build`, `make monitor`, and `make flash-monitor` are there too;
override the serial device with `make flash PORT=...`.)

If you're starting from a previously-flashed device, wipe it first:

```bash
make erase && make flash
```

Once running, you can also wipe everything from the device itself: **tap
BOOT → settings → reset → factory reset → Confirm**.

## Pairing

To pair your device with Claude, first enable developer mode (**Help →
Troubleshooting → Enable Developer Mode**). Then, open the Hardware Buddy
window in **Developer → Open Hardware Buddy…**, click **Connect**, and pick
your device from the list. macOS will prompt for Bluetooth permission on
first connect; grant it.

<p align="center">
  <img src="docs/menu.png" alt="Developer → Open Hardware Buddy… menu item" width="420">
  <img src="docs/hardware-buddy-window.png" alt="Hardware Buddy window with Connect button and folder drop target" width="420">
</p>

Once paired, the bridge auto-reconnects whenever both sides are awake.

If discovery isn't finding the device:

- Make sure it's awake (press a button or double-tap the screen)
- Check the INFO → BLUETOOTH page on the device (swipe up from home, then
  swipe left to the BLUETOOTH page) for the advertising / connection state

## Controls

The screen is a touch panel; the two physical buttons (BOOT and the
AXP2101 power button) keep single, fixed meanings.

|                      | Normal                  | Pet         | Info        | Approval    |
|----------------------|-------------------------|-------------|-------------|-------------|
| **Swipe right**      | next character          | prev page   | prev page   | **approve** |
| **Swipe left**       | prev character          | next page   | next page   | **deny**    |
| **Swipe down**       | pet view                | info view   | home        |             |
| **Swipe up**         | info view               | home        | pet view    |             |
| **Tap HUD**          | open transcript log     |             |             |             |
| **BOOT** (short)     | menu                    | menu        | menu        | menu        |
| **BOOT** (hold)      | screenshot to SD        | ″           | ″           | ″           |
| **Power** (short)    | toggle screen off       | ″           | ″           |             |
| **Power** (~6s)      | hard power off          | ″           | ″           | ″           |
| **Shake**            | dizzy                   |             |             | —           |
| **Face-down**        | nap (energy refills)    |             |             |             |

On battery the screen powers off after 30s of no interaction (kept on
while an approval prompt is up); on USB an idle clock face takes over
instead. A button press or a double-tap on the panel wakes it. See
[docs/USER-GUIDE.md](docs/USER-GUIDE.md) for the full gesture map, menus,
and automatic behaviours.

## ASCII pets

```
  ▐▛███▜▌
 ▝▜█████▛▘
   ▘▘ ▝▝
```

Nineteen pets, each with seven animations (sleep, idle, busy, attention,
celebrate, dizzy, heart). Settings → "ascii pet" cycles them with a
counter. Choice persists to NVS. On home, a swipe left / right cycles
them too.

### Block-art drawing palette

Pet bodies are drawn with Unicode **Block Elements** (U+2580–U+259F).
On-device they are not font glyphs — `src/blockart.cpp` fills each cell
with exact rectangles, so adjacent blocks tile seamlessly. Compose art on
a monospace grid (each character = one 6×8 cell, rendered 18×32 px); any
non-block character falls back to the GFX font, so ASCII faces mix in
freely. See `src/buddies/ember.cpp` for a worked example.

| Glyphs            | Code                  | Coverage                               |
|-------------------|-----------------------|----------------------------------------|
| `█`               | U+2588                | full cell                              |
| `▀` / `▄`         | U+2580 / 2584         | top / bottom half                      |
| `▌` / `▐`         | U+258C / 2590         | left / right half                      |
| `▁ ▂ ▃ ▄ ▅ ▆ ▇ █` | U+2581–2588           | bottom ⅛ … 8⁄8                         |
| `▏ ▎ ▍ ▌ ▋ ▊ ▉ █` | U+258F→2588           | left ⅛ … 8⁄8                           |
| `▔` / `▕`         | U+2594 / 2595         | top ⅛ / right ⅛                        |
| `▘ ▝ ▖ ▗`         | U+2598/259D/2596/2597 | one quadrant: TL TR BL BR              |
| `▚ ▞`             | U+259A / 259E         | diagonal pairs: TL+BR / TR+BL          |
| `▛ ▜ ▙ ▟`         | U+259B/259C/2599/259F | three quadrants (notch at BR BL TR TL) |
| `░ ▒ ▓`           | U+2591–2593           | 25 / 50 / 75 % shade (color blend)     |

Quick recipes — think of each cell as four quadrants; the *missing*
quadrant of `▛▜▙▟` (BR, BL, TR, TL respectively) is what rounds a corner:

```
rounded shoulders:   ▐▛████▜▌
rounded base:        ▝▜████▛▘
feet / toes:           ▘▘ ▝▝      single quadrants read as tiny feet
flame tip:              ▗▟▖       quadrant + three-quadrant stack
half-step outline:   ▗▄▄▄▄▄▄▖     eighths make soft slopes
```

Useful sources: [Wikipedia: Block Elements](https://en.wikipedia.org/wiki/Block_Elements),
the official [Unicode chart PDF](https://www.unicode.org/charts/PDF/U2580.pdf).
Print them all in a terminal:

```sh
python3 -c "print(''.join(chr(c) for c in range(0x2580, 0x25A0)))"
```

## GIF pets

If you want a custom GIF character instead of an ASCII buddy, drag a
character pack folder onto the drop target in the Hardware Buddy window. The
app streams it over BLE and the device switches to GIF mode live. **Settings
→ reset → delete char** reverts to ASCII mode.

A character pack is a folder with `manifest.json` and 96px-wide GIFs:

```json
{
  "name": "bufo",
  "colors": {
    "body": "#6B8E23",
    "bg": "#000000",
    "text": "#FFFFFF",
    "textDim": "#808080",
    "ink": "#000000"
  },
  "states": {
    "sleep": "sleep.gif",
    "idle": [
      "idle_0.gif",
      "idle_1.gif",
      "idle_2.gif"
    ],
    "busy": "busy.gif",
    "attention": "attention.gif",
    "celebrate": "celebrate.gif",
    "dizzy": "dizzy.gif",
    "heart": "heart.gif"
  }
}
```

State values can be a single filename or an array. Arrays rotate: each
loop-end advances to the next GIF, useful for an idle activity carousel so
the home screen doesn't loop one clip forever.

GIFs are 96px wide; they're drawn centred in the buddy area of the 368×448
panel. Crop tight to the character — transparent margins shrink the visible
sprite. `tools/prep_character.py` handles the resize: feed it source
GIFs at any sizes and it produces a 96px-wide set where the character is the
same scale in every state.

The pack lands on the microSD card if one is inserted, otherwise on the
internal LittleFS partition. Keep it small either way — BLE pushes at
~3 KB/s, and `gifsicle --lossy=80 -O3 --colors 64` typically cuts 40–60%.
State entries can also be pixel-art **SVG** files, which are far smaller
than GIFs — see [docs/USER-GUIDE.md](docs/USER-GUIDE.md) and
`characters/claude-svg/`.

See `characters/bufo/` for a working GIF example.

If you're iterating on a character and would rather skip the BLE round-trip,
`tools/usb_xfer_send.sh characters/bufo` (or `make char-usb
CHAR=characters/bufo`) pushes the pack through the same transfer protocol
over USB-CDC, and `tools/flash_character.py characters/bufo` stages it into
`data/` and runs `pio run -t uploadfs` directly over USB.

## The seven states

| State       | Trigger                     | Feel                        |
|-------------|-----------------------------|-----------------------------|
| `sleep`     | bridge not connected        | eyes closed, slow breathing |
| `idle`      | connected, nothing urgent   | blinking, looking around    |
| `busy`      | sessions actively running   | sweating, working           |
| `attention` | approval pending            | alert, agitated             |
| `celebrate` | session completes; level up (every 50K tokens) | confetti, bouncing |
| `dizzy`     | you shook the device        | spiral eyes, wobbling       |
| `heart`     | approved in under 5s        | floating hearts             |

## Project layout

```
src/
  main.cpp       — loop, state machine, UI screens
  buddy.cpp      — ASCII species dispatch + render helpers
  buddies/       — one file per species, seven anim functions each
  ble_bridge.cpp — Nordic UART service, line-buffered TX/RX
  character.cpp  — GIF / SVG character pack decode + render
  hal/           — board drivers: display, touch, power, audio, storage, IMU
  data.h         — wire protocol, JSON parse
  xfer.h         — folder push receiver
  stats.h        — NVS-backed stats, settings, owner, species choice
characters/      — example GIF character packs
tools/           — generators and converters
```

## Availability

The BLE API is only available when the desktop apps are in developer mode
(**Help → Troubleshooting → Enable Developer Mode**). It's intended for
makers and developers and isn't an officially supported product feature.

## Palette

Brand accent — the Claude terracotta orange, sampled from the desktop UI:

| Name          | HEX       | RGB            | RGB565 (display) |
|---------------|-----------|----------------|------------------|
| Claude orange | `#DE7643` | `222, 118, 67` | `0xDBA8`         |

The display HAL works in 16-bit RGB565, so use `0xDBA8` in `gfx.*` calls
(`#DE7643` is the 24-bit source). Currently used for the **level badge**;
reserved as the primary accent for UI styling.
