#pragma once
#include <stdint.h>
#include <FS.h>

// Hybrid storage HAL: prefers the microSD slot, falls back to the on-flash
// LittleFS partition, falls back to "no storage" so the rest of the app
// can keep working without a card or a formatted flash region.
//
// Why hybrid:
//   • SD gives gigabytes of room — handy for many GIF character packs.
//   • Users can pop the card out to load packs on their desktop.
//   • LittleFS is always there, no extra hardware required.
//   • The xfer.h folder-push transport (Stage 4c) needs SOMEWHERE to put
//     the bytes; this layer hides the choice.
//
// Backed by the standard Arduino-ESP32 SDMMC and LittleFS libraries; both
// expose the same `fs::FS` interface, so callers get one `fs::FS&` and use
// it uniformly. Path semantics are the same on either backend
// (`/characters/<pack>/<file>`).

enum StorageBackend : uint8_t {
  STORAGE_NONE = 0,
  STORAGE_SD,
  STORAGE_LFS,
};

// Try SD first, then LittleFS. Returns true if either mount succeeds.
// Safe to call multiple times — re-init is a no-op once a backend is up.
bool storageInit();

// Which backend won. Cached after storageInit().
StorageBackend storageBackend();
const char*    storageBackendName();   // "SD" / "LittleFS" / "none"

// Reference to the active filesystem. Asserts that storageInit() ran first
// and returned true — calling this before init returns LittleFS by default
// (a dead reference if LittleFS itself never mounted, but the FS API
// returns false on every op so callers degrade gracefully).
fs::FS& storageFS();

// Capacity introspection — returns 0 when STORAGE_NONE.
uint64_t storageTotalBytes();
uint64_t storageUsedBytes();
