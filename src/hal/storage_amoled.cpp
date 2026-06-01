#include "storage.h"
#include "../board_pins.h"
#include <Arduino.h>
#include <SD_MMC.h>
#include <LittleFS.h>

// Hybrid storage HAL. SD probe → LittleFS fallback. See storage.h.

static StorageBackend _backend = STORAGE_NONE;

bool storageInit() {
  if (_backend != STORAGE_NONE) return true;

  // ── SD first ──
  // Waveshare wires the SDMMC bus as 1-bit (CLK / CMD / DATA0 only — there
  // is no D1/D2/D3 broken out). setPins() with three args puts the driver
  // in 1-bit mode automatically.
  SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
  if (SD_MMC.begin("/sdcard", /*mode1bit*/ true,
                   /*format_if_mount_failed*/ false,
                   SDMMC_FREQ_DEFAULT, /*max_files*/ 5)) {
    _backend = STORAGE_SD;
    uint64_t total = SD_MMC.totalBytes();
    uint64_t used  = SD_MMC.usedBytes();
    Serial.printf("[storage] SD ok  total=%lluMB used=%lluMB\n",
                  (unsigned long long)(total / (1024ULL * 1024ULL)),
                  (unsigned long long)(used  / (1024ULL * 1024ULL)));
    return true;
  }
  Serial.println("[storage] no SD card — falling back to LittleFS");

  // ── LittleFS fallback ──
  // format-if-mount-failed = true so a fresh board comes up healthy without
  // a separate fs-flash step. Cost: first boot after wipe takes a couple
  // hundred ms longer while the partition gets formatted.
  if (LittleFS.begin(/*format_if_mount_failed*/ true)) {
    _backend = STORAGE_LFS;
    Serial.printf("[storage] LittleFS ok  total=%uKB used=%uKB\n",
                  (unsigned)(LittleFS.totalBytes() / 1024),
                  (unsigned)(LittleFS.usedBytes()  / 1024));
    return true;
  }

  _backend = STORAGE_NONE;
  Serial.println("[storage] FAILED — no SD card and LittleFS mount failed");
  return false;
}

StorageBackend storageBackend() { return _backend; }

const char* storageBackendName() {
  switch (_backend) {
    case STORAGE_SD:  return "SD";
    case STORAGE_LFS: return "LittleFS";
    default:          return "none";
  }
}

fs::FS& storageFS() {
  // Conditional reference: assign through a named reference to avoid the
  // "different reference types in conditional" error gcc would throw.
  if (_backend == STORAGE_SD) {
    fs::FS& fs = SD_MMC;
    return fs;
  }
  fs::FS& fs = LittleFS;   // also the dead-fallback when _backend == NONE
  return fs;
}

uint64_t storageTotalBytes() {
  switch (_backend) {
    case STORAGE_SD:  return SD_MMC.totalBytes();
    case STORAGE_LFS: return LittleFS.totalBytes();
    default:          return 0;
  }
}

uint64_t storageUsedBytes() {
  switch (_backend) {
    case STORAGE_SD:  return SD_MMC.usedBytes();
    case STORAGE_LFS: return LittleFS.usedBytes();
    default:          return 0;
  }
}
