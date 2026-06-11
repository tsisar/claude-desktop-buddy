# Changelog

Firmware version is defined in `src/version.h` and reported in the
`cmd:status` ack as `data.fw`.

## 0.9.0 — 2026-06-11

First versioned release of the Waveshare ESP32-S3-Touch-AMOLED-1.8 port.
Highlights of the audit-fix pass:

### Fixed
- Link drop with a pending prompt no longer pins the approval screen,
  block auto screen-off / nap, or let a swipe answer an abandoned prompt.
- Permission decisions (approve/deny) now go out over both USB-CDC and
  BLE — USB-connected hosts used to receive nothing.
- Interrupted character transfers abort cleanly (link loss, host restart,
  delete-char mid-push): the half-written file is closed before any wipe,
  the progress screen unpins, and out-of-sequence transfer commands are
  explicitly nacked instead of silently swallowed.
- BLE pairing actually uses DisplayOnly + passkey now; it silently ran
  "Just Works" before because the IO capability was never set.
- BLE RX ring overflow and over-long JSON lines are detected and resynced
  on a line boundary instead of feeding garbled frames to the parser.
- `cmd:status` could read past its reply buffer with max-length names.
- Bridge time sync is sanity-checked (epoch >= 2024, |tz| <= 14 h).
- Factory reset no longer logs a false watchdog-hang on the next boot.
- Failed screenshots no longer strand truncated BMPs on storage.

### Power
- Blanked panel now skips the full canvas recompose + 322 KB QSPI flush,
  drops the CPU to 80 MHz, puts the SH8601 into sleep and the FT3168
  into monitor mode (double-tap wake still works).
- Speaker amp + I2S clock tree gate off after 250 ms of audio quiet.
- BLE advertises at 0.5-1 s intervals instead of the fast default.
- IMU accelerometer ODR 1000 Hz -> 125 Hz.
- Clock face acts as an AOD: lowest brightness tier + ±4 px pixel shift
  (AMOLED burn-in protection); skips redraws when the second is unchanged.
- Stats flush once at 15% battery so the PMU hard-cut can't eat progress.

### Changed
- stats/xfer/data modules converted from header-only statics to real
  translation units (removes a live duplicate-state hazard).
- GIF frames blit rows directly into the canvas framebuffer.
- Shared overlay helpers for the species animations (orbit stars, hearts,
  confetti, dots, "!") replace per-file copy-paste.
- Host tools: DST-correct time sync, real error reporting on failed
  installs, declared Pillow dep, unified size cap, ttyACM port discovery.
- Docs synced with shipped behavior (approve/deny swipe direction was
  documented INVERTED in the port doc).
- `lib_deps` pinned to exact hardware-tested versions; CI build workflow.
