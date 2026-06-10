#pragma once
#include <stddef.h>

// Screenshot-to-storage (3i.5+). Saves the canvas — i.e. the last frame
// pushed to the panel — as a 24-bit BMP under /screenshots/ on the active
// storage backend (SD card preferred, LittleFS fallback; see hal/storage.h).
// BMP because it needs no encoder: header + raw BGR888 rows, readable by
// anything that can open an image.
//
// Two triggers, both capturing any state (home, menus, modals): BOOT
// long-press (main.cpp button block) and the xfer protocol's
// {"cmd":"shot"} over USB/BLE (xfer.h). Blocking for the duration of the
// write (~0.3 s for 368x448 on SD) — acceptable for a debug/QA affordance.
//
// Returns true and logs the path on success; false if no canvas, no
// storage, or the write failed. When pathOut is given, the saved path is
// copied there (empty string on failure).
bool screenshotSave(char* pathOut = nullptr, size_t pathLen = 0);