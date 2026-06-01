# User guide — Waveshare ESP32-S3-Touch-AMOLED-1.8 port

How the device behaves from the user's side: what the screens show, when
they appear, which gestures and buttons do what.

## Screens

The home view shows your **buddy** (ASCII art or a GIF character) in the
upper half and a transcript HUD with the latest lines from Claude in the
lower 130 pixels. A **battery widget** sits in the top-right corner.

Other views overlay or replace home depending on what's happening:

| View                  | When it appears                                                                  |
|-----------------------|----------------------------------------------------------------------------------|
| **Home buddy + HUD**  | Default. Live Claude session activity or "No Claude connected"                   |
| **Clock face**        | On USB power, RTC synced, no live sessions, no prompt, no menu — full-screen HH:MM, seconds, weekday/date |
| **Approval screen**   | When Claude needs permission for a tool call. Shows tool name + hint + timer     |
| **Passkey screen**    | During BLE pairing — full-screen 6-digit code to type on the desktop             |
| **PET pages** (2)     | `stats` (mood / fed / energy / level / counters) and `how-to` (gameplay tips)    |
| **INFO pages** (6)    | About / Controls / Claude session info / Device (battery, uptime, heap) / Bluetooth / Credits |
| **Installing**        | While Claude Desktop is pushing a custom character pack — progress bar + KB count |
| **Menu / Settings / Reset / Confirm** | Modal panels overlay everything below                                |

## Buddy states

The buddy reacts to what Claude is doing:

| State        | When                                                  |
|--------------|-------------------------------------------------------|
| **sleep**    | No bridge connection, or just after waking the screen |
| **idle**     | Bridge connected, no running sessions                 |
| **busy**     | 3 or more sessions actively generating                |
| **attention**| A session is waiting on a permission prompt           |
| **celebrate**| A session just completed (3 s one-shot); also fires on level-up |
| **dizzy**    | You shook the device                                  |
| **heart**    | Approved a prompt in under 5 seconds                  |

A 12-second SLEEP-hold is armed every time the screen wakes, so the
wake-up animation gets airtime before the buddy snaps to its current
state.

## Gestures

Everything that used to be M5's A/B buttons is touch-based now.

| Gesture                  | Where           | What it does                                                       |
|--------------------------|-----------------|--------------------------------------------------------------------|
| **swipe right**          | home (prompt)   | Approve the permission request                                     |
| **swipe left**           | home (prompt)   | Deny the permission request                                        |
| **swipe right**          | home (no prompt)| Next pet / character                                               |
| **swipe left**           | home (no prompt)| Previous pet / character                                           |
| **swipe left**           | PET / INFO      | Next page                                                          |
| **swipe right**          | PET / INFO      | Previous page                                                      |
| **swipe up**             | home            | Open the menu                                                      |
| **swipe up**             | any modal       | Close everything back to home                                      |
| **swipe down**           | home            | Cycle display mode: NORMAL → PET → INFO → NORMAL                   |
| **swipe down**           | any modal       | Step back one level                                                |
| **tap on row**           | menu / settings / reset | Activate that row                                          |
| **tap outside panel**    | menu / settings / reset | Close current modal                                        |
| **tap on HUD area**      | home (no prompt)| Scroll the transcript back one line (`-N` indicator appears)       |

## Physical buttons

| Button + duration    | What it does                                                                      |
|----------------------|-----------------------------------------------------------------------------------|
| **BOOT short**       | In a menu: move highlight up. On home: cycle display mode (NORMAL → PET → INFO)   |
| **BOOT short**       | On PET / INFO: next page                                                          |
| **BOOT long** (≥600ms) | Activate the highlighted row. On home: open the menu                            |
| **PWRON short**      | On home: toggle screen off / on. In a menu: move highlight down                   |
| **PWRON short**      | On PET / INFO: previous page                                                      |
| **PWRON long** (~6 s)| Hardware power-off (AXP2101 cuts the rails)                                       |

Touch and physical buttons run in parallel — pick whichever is easier.
The button cursor (highlighted row) lives behind a touch tap too, so
mid-menu you can switch between the two without losing your place.

## Automatic behaviours

The device tries to be invisible most of the time. These all run on
their own without you touching anything:

- **Clock face** appears when the device is parked: on USB, no live
  sessions, no prompt, no menu, in DISP_NORMAL, and the RTC has been
  time-synced by the bridge at least once. Any change (prompt arrives,
  Claude starts working, you open a menu, USB unplug) brings the buddy
  back.
- **Auto screen-off** at 30 seconds of no input on battery. On USB the
  screen stays lit (the clock face takes over). Any gesture / button /
  prompt arrival / shake wakes it back up.
- **Face-down nap.** Put the device screen-down on a table for ~0.3 s
  and it dims, paused, and starts accumulating nap time. Pick it up and
  the energy meter refills, the buddy wakes. Skipped while an approval
  prompt is up (you're probably reading it).
- **Shake** the device → buddy goes DIZZY for 2 seconds.
- **Beep** on prompt arrival (1200 Hz), approve (2400 Hz), deny (600 Hz),
  menu cycle (1800 Hz). Mute via Settings → sound. (Audio is currently
  silent on hardware while a codec issue gets investigated — the toggle
  works once the codec lands.)

## Menus

**Swipe up** (or BOOT long on home) opens the main menu.

| Main menu     | What it does                                              |
|---------------|-----------------------------------------------------------|
| settings      | Open the settings sub-menu                                |
| turn off      | Black the screen (PWRON short or a tap wakes it back)     |
| help / about  | (Stub — INFO pages cover these for now)                   |
| demo          | Cycle fake scenarios — handy without a bridge connected   |
| close         | Close the menu                                            |

### Settings

| Setting       | Value column   | Notes                                                                  |
|---------------|----------------|------------------------------------------------------------------------|
| brightness    | `N/4`          | Tap to cycle 0..4 (40..230 panel brightness)                           |
| sound         | on / off       | Beep gate                                                              |
| bluetooth     | on / off       | Stored preference; BLE stays advertising regardless on this build      |
| wifi          | on / off       | Stored preference; no WiFi stack linked                                |
| led           | on / off       | Stored preference; AMOLED board has no separate LED                    |
| transcript    | on / off       | HUD on home                                                            |
| clock rot     | auto / port / land | Affects M5 landscape rotation only; AMOLED is portrait-only         |
| ascii pet     | `N/M`          | Cycle through species. With a GIF pack installed, the last slot is GIF |
| reset         | →              | Opens the reset sub-menu                                               |
| back          | →              | Return to main menu                                                    |

### Reset

| Action            | What it wipes                                                                |
|-------------------|------------------------------------------------------------------------------|
| delete char       | The installed GIF character pack (back to ASCII species)                     |
| factory reset     | NVS + BLE bonds + character pack, then restarts the board                    |
| back              | Return to settings                                                           |

Both destructive actions open a `Cancel / Confirm` modal — tap outside
or swipe to cancel, BOOT long on Confirm or tap it to commit.

## Custom GIF characters

The 18 ASCII species (capybara, cat, dragon, octopus, owl, penguin,
…) are built in. You can also push a custom GIF pack from the
Claude desktop:

1. **Help → Troubleshooting → Enable Developer Mode** on the desktop.
2. **Developer → Open Hardware Buddy…** and connect to the device.
3. Drag a character-pack folder onto the drop target. The pack is a
   directory with `manifest.json` (palette + state mapping) and a few
   96px-wide GIFs (`sleep.gif`, `idle.gif`, …, `heart.gif`). See
   `characters/bufo/` in this repo for a working example.
4. While the upload runs the device shows an `installing N/M KB`
   progress bar with `don't unplug` underneath. ~3 KB/s over BLE.
5. When the bar finishes, the buddy switches to the GIF character.
   Settings → "ascii pet" now cycles `species 1..18 → GIF → 1`.

The pack lives on the device's storage (SD card if a microSD is
inserted, otherwise the internal LittleFS partition). One pack at a
time — pushing a new one wipes the old.

## Approval flow

When Claude needs permission for a tool call:

1. The screen wakes (if asleep), beeps once.
2. The approval view shows: **timer** (`approve? Ns`, turns red after
   10 s), **tool name** centered (auto-sized 5/4/3 depending on length),
   **wrapped hint** below, and **swipe hints** at the bottom: `<
   DENY | APPROVE >`.
3. **Swipe right** approves → bottom row shows `sent…`. **Swipe left**
   denies → same. Buddy reacts: HEART if you were under 5 seconds,
   regular state otherwise.
4. The desktop forwards your decision to Claude.

If you ignore the prompt for too long, Claude eventually times it out
on its end — the device doesn't enforce a deadline.

## Bluetooth pairing

The first time the desktop connects:

1. The screen wakes and shows a **6-digit passkey** full-screen with
   "BLUETOOTH PAIRING" / "enter on desktop".
2. Type the digits into the macOS / Windows pairing prompt.
3. The link is now AES-CCM encrypted (you'll see `ble encrypted` on
   INFO → CLAUDE). The desktop reconnects silently next time.

To re-pair (e.g. you switched desktops): Reset → Factory reset (also
clears the stored bond), or send `{"cmd":"unpair"}` from the bridge.

## Recovery / what to do if something looks stuck

| Symptom                                | Try this                                                             |
|----------------------------------------|----------------------------------------------------------------------|
| Screen is dark on battery              | Tap, swipe, press a button, or shake — anything wakes it             |
| Touch doesn't respond after reflash    | Power-cycle. The XCA9554 expander reset pulse runs in `powerInit()`  |
| Clock face doesn't appear              | Need: USB plugged in + no sessions + no prompt + RTC synced by bridge|
| Buddy stuck on SLEEP forever           | Bridge isn't sending heartbeats. Check desktop is connected          |
| Custom GIF didn't appear after push    | `char_end ok=false` means the pack landed but parsing failed — check manifest.json |
| Can't pair                             | Factory reset clears the stored bond, then re-pair fresh             |
| Hard power-off                         | Hold the PWRON button for ~6 seconds                                 |
