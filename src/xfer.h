#pragma once
#include <Arduino.h>
#include <FS.h>
#include <ArduinoJson.h>
#include <mbedtls/base64.h>
#include "hal/storage.h"
#include "hal/power.h"
#include "hal/battery.h"
#include "ble_bridge.h"
#include "stats.h"
#include "buddy.h"
#include "debug.h"

// AMOLED port of xfer.h. Same wire protocol as the M5 build (REFERENCE.md):
// cmd:char_begin / cmd:file / cmd:chunk / cmd:file_end / cmd:char_end,
// plus the housekeeping cmd:status / name / owner / species / unpair.
//
// Differences from M5:
//   • LittleFS replaced by hal/storage.h — files go onto SD if a card is
//     mounted, otherwise the LittleFS partition. storageFS() gives us a
//     fs::FS& that both backends inherit from, so this file stays
//     filesystem-agnostic.
//   • Battery / VBUS readouts go through hal/power.h instead of M5.Axp.
//     mA isn't exposed by XPowersLib here, so cmd:status reports 0 for
//     current. Desktop tolerates the missing field.
//   • characterClose() / characterInit() are externs — stubbed in main.cpp
//     until Stage 4d brings the GIF renderer online.
//
// Header-only with file-static state — include from EXACTLY ONE
// translation unit (main.cpp).

static File     _xFile;
static uint32_t _xExpected = 0, _xWritten = 0;
static char     _xCharName[24] = "";
static bool     _xActive = false;
static uint32_t _xTotal = 0, _xTotalWritten = 0;

// Character renderer hooks — provided by main.cpp (stubs for now, real impl
// in Stage 4d). Declared as plain functions so the linker can swap the
// stub for the real implementation without touching this file.
void characterClose();
bool characterInit(const char* name);

// Buddy / GIF mode flags — owned by main.cpp.
extern bool buddyMode;
extern bool gifAvailable;

static void _xAck(const char* what, bool ok, uint32_t n = 0) {
  char b[64];
  int len = snprintf(b, sizeof(b),
    "{\"ack\":\"%s\",\"ok\":%s,\"n\":%lu}\n",
    what, ok ? "true" : "false", (unsigned long)n);
  VWRITE(b, len);
  bleWrite((const uint8_t*)b, len);
}

// Recursively remove a directory's contents (one level deep — char packs
// don't nest). Returns total bytes reclaimed.
static uint32_t _xWipeDir(const char* dir) {
  fs::FS& fs = storageFS();
  File d = fs.open(dir);
  if (!d || !d.isDirectory()) { fs.mkdir(dir); return 0; }
  uint32_t freed = 0;
  File f = d.openNextFile();
  while (f) {
    freed += f.size();
    char p[96];
    snprintf(p, sizeof(p), "%s/%s", dir, f.name());
    f.close();
    fs.remove(p);
    f = d.openNextFile();
  }
  d.close();
  return freed;
}

// Wipe everything under /characters/ — only one pack lives on the device
// at a time, so installing a new pack with a different name would
// otherwise leak the old one's bytes.
static uint32_t _xWipeAllChars() {
  fs::FS& fs = storageFS();
  File root = fs.open("/characters");
  if (!root || !root.isDirectory()) { fs.mkdir("/characters"); return 0; }
  uint32_t freed = 0;
  File sub = root.openNextFile();
  while (sub) {
    if (sub.isDirectory()) {
      char p[80];
      snprintf(p, sizeof(p), "/characters/%s", sub.name());
      sub.close();
      freed += _xWipeDir(p);
      fs.rmdir(p);
    } else {
      sub.close();
    }
    sub = root.openNextFile();
  }
  root.close();
  return freed;
}

inline bool xferCommand(JsonDocument& doc) {
  const char* cmd = doc["cmd"];
  if (!cmd) return false;

  if (strcmp(cmd, "name") == 0) {
    const char* n = doc["name"];
    if (n) petNameSet(n);
    _xAck("name", n != nullptr);
    return true;
  }

  if (strcmp(cmd, "species") == 0) {
    uint8_t idx = doc["idx"] | 0xFF;
    speciesIdxSave(idx);
    buddyMode = !(gifAvailable && idx == 0xFF);
    if (buddyMode) buddySetSpeciesIdx(idx);
    _xAck("species", true);
    return true;
  }

  if (strcmp(cmd, "unpair") == 0) {
    bleClearBonds();
    _xAck("unpair", true);
    return true;
  }

  if (strcmp(cmd, "owner") == 0) {
    const char* n = doc["name"];
    if (n) ownerSet(n);
    _xAck("owner", n != nullptr);
    return true;
  }

  if (strcmp(cmd, "status") == 0) {
    // Battery + system telemetry for the desktop's stats panel. mA isn't
    // exposed by XPowersLib here — report 0; desktop already tolerates
    // missing fields.
    int pct  = powerOk() ? batteryPercent()      : 0;
    int mV   = powerOk() ? batteryMilliVolts()   : 0;
    bool usb = onUsb();
    uint64_t fsTotal = storageTotalBytes();
    uint64_t fsFree  = fsTotal - storageUsedBytes();
    char b[320];
    int len = snprintf(b, sizeof(b),
      "{\"ack\":\"status\",\"ok\":true,\"n\":0,\"data\":{"
      "\"name\":\"%s\",\"owner\":\"%s\",\"sec\":%s,"
      "\"bat\":{\"pct\":%d,\"mV\":%d,\"mA\":0,\"usb\":%s},"
      "\"sys\":{\"up\":%lu,\"heap\":%u,\"fsFree\":%llu,\"fsTotal\":%llu},"
      "\"stats\":{\"appr\":%u,\"deny\":%u,\"vel\":%u,\"nap\":%lu,\"lvl\":%u}"
      "}}\n",
      petName(), ownerName(), bleSecure() ? "true" : "false",
      pct, mV, usb ? "true" : "false",
      millis() / 1000, ESP.getFreeHeap(),
      (unsigned long long)fsFree, (unsigned long long)fsTotal,
      stats().approvals, stats().denials, statsMedianVelocity(),
      (unsigned long)stats().napSeconds, stats().level);
    VWRITE(b, len);
    bleWrite((const uint8_t*)b, len);
    return true;
  }

  if (strcmp(cmd, "char_begin") == 0) {
    const char* name = doc["name"] | "pet";
    _xTotal = doc["total"] | 0;

    // Fit check BEFORE touching the filesystem so a failed sizing leaves
    // whatever's currently installed intact.
    uint64_t free       = storageTotalBytes() - storageUsedBytes();
    uint32_t reclaimable = 0;
    {
      fs::FS& fs = storageFS();
      File r = fs.open("/characters");
      if (r && r.isDirectory()) {
        File s = r.openNextFile();
        while (s) {
          if (s.isDirectory()) {
            File f = s.openNextFile();
            while (f) { reclaimable += f.size(); f.close(); f = s.openNextFile(); }
          }
          s.close(); s = r.openNextFile();
        }
        r.close();
      }
    }
    uint64_t available = free + reclaimable;
    // 4KB headroom for filesystem metadata — LittleFS isn't byte-for-byte.
    if (_xTotal > 0 && (uint64_t)_xTotal + 4096 > available) {
      char b[128];
      int len = snprintf(b, sizeof(b),
        "{\"ack\":\"char_begin\",\"ok\":false,\"n\":%llu,"
        "\"error\":\"need %luK, have %lluK\"}\n",
        (unsigned long long)available,
        (unsigned long)(_xTotal / 1024),
        (unsigned long long)(available / 1024));
      VWRITE(b, len);
      bleWrite((const uint8_t*)b, len);
      return true;
    }

    strncpy(_xCharName, name, sizeof(_xCharName) - 1);
    _xCharName[sizeof(_xCharName) - 1] = 0;
    characterClose();
    _xWipeAllChars();
    char dir[48];
    snprintf(dir, sizeof(dir), "/characters/%s", _xCharName);
    storageFS().mkdir(dir);
    _xTotalWritten = 0;
    _xActive = true;
    _xAck("char_begin", true);
    return true;
  }

  // permission isn't ours, but we want to claim every other unknown cmd
  // once a transfer is mid-flight so the heartbeat path doesn't try to
  // parse {"cmd":"chunk", ...} as state.
  if (!_xActive) return strcmp(cmd, "permission") != 0;

  if (strcmp(cmd, "file") == 0) {
    const char* path = doc["path"];
    _xExpected = doc["size"] | 0;
    _xWritten = 0;
    if (!path) { _xAck("file", false); return true; }
    char full[96];
    snprintf(full, sizeof(full), "/characters/%s/%s", _xCharName, path);
    _xFile = storageFS().open(full, "w");
    _xAck("file", (bool)_xFile);
    return true;
  }

  if (strcmp(cmd, "chunk") == 0) {
    const char* b64 = doc["d"];
    if (!b64 || !_xFile) {
      Serial.printf("[xfer] chunk: missing %s\n", b64 ? "file" : "data");
      _xAck("chunk", false);
      return true;
    }
    uint8_t buf[300];
    size_t outLen = 0;
    size_t b64Len = strlen(b64);
    int rc = mbedtls_base64_decode(buf, sizeof(buf), &outLen,
                                   (const uint8_t*)b64, b64Len);
    if (rc != 0) {
      Serial.printf("[xfer] chunk: base64 fail rc=%d in=%u out=%u\n",
                    rc, (unsigned)b64Len, (unsigned)outLen);
      _xAck("chunk", false);
      return true;
    }
    size_t wrote = _xFile.write(buf, outLen);
    // yield() lets FreeRTOS task scheduler run — LittleFS flash erase can
    // block for hundreds of ms, and without yielding the IDLE task never
    // gets to reset the task WDT. WDT-panic was the suspected cause of
    // the second-chunk hang during the bufo test push.
    yield();
    if (wrote != outLen) {
      Serial.printf("[xfer] chunk: write short wrote=%u expected=%u "
                    "free=%llu\n",
                    (unsigned)wrote, (unsigned)outLen,
                    (unsigned long long)(storageTotalBytes() - storageUsedBytes()));
      _xAck("chunk", false, _xWritten);
      return true;
    }
    _xWritten += outLen;
    _xTotalWritten += outLen;
    // Ack every chunk: filesystem writes can block on flash erase, and
    // the USB-CDC RX ring buffer is small. The sender waits for each ack
    // so the buffer never overruns.
    _xAck("chunk", true, _xWritten);
    return true;
  }

  if (strcmp(cmd, "file_end") == 0) {
    bool ok = _xFile && (_xWritten == _xExpected || _xExpected == 0);
    if (_xFile) _xFile.close();
    _xAck("file_end", ok, _xWritten);
    return true;
  }

  if (strcmp(cmd, "char_end") == 0) {
    _xActive = false;
    bool ok = characterInit(_xCharName);
    if (ok) { buddyMode = false; gifAvailable = true; speciesIdxSave(0xFF); }
    _xAck("char_end", ok);
    return true;
  }

  return false;
}

inline bool     xferActive()   { return _xActive; }
inline uint32_t xferProgress() { return _xTotalWritten; }
inline uint32_t xferTotal()    { return _xTotal; }

// Wipe /characters/ — backs the "delete char" reset path.
inline void xferDeleteAll() {
  characterClose();
  _xWipeAllChars();
  gifAvailable = false;
  buddyMode = true;
  // Reset species sentinel so future load doesn't try GIF mode.
  speciesIdxSave(0);
}
