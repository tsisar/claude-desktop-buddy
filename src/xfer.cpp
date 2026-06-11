#include "xfer.h"
#include <Arduino.h>
#include <FS.h>
#include <mbedtls/base64.h>
#include "hal/storage.h"
#include "hal/power.h"
#include "hal/battery.h"
#include "ble_bridge.h"
#include "stats.h"
#include "buddy.h"
#include "debug.h"
#include "screenshot.h"

// AMOLED port of the transfer handler. Differences from M5:
//   • LittleFS replaced by hal/storage.h — files go onto SD if a card is
//     mounted, otherwise the LittleFS partition. storageFS() gives us a
//     fs::FS& that both backends inherit from, so this file stays
//     filesystem-agnostic.
//   • Battery / VBUS readouts go through hal/power.h instead of M5.Axp.
//     mA isn't exposed by XPowersLib here, so cmd:status reports 0 for
//     current. Desktop tolerates the missing field.
//   • characterClose() / characterInit() are externs implemented by
//     character.cpp — char_end loads the freshly-pushed pack for real.

static File     _xFile;
static uint32_t _xExpected = 0, _xWritten = 0;
static char     _xCharName[24] = "";
static bool     _xActive = false;
static uint32_t _xTotal = 0, _xTotalWritten = 0;

// Close out any in-flight transfer. Called when a new char_begin arrives
// while a previous transfer never finished (host crashed / link dropped
// mid-push) and from xferDeleteAll. Without this the stale _xFile handle
// stayed open and the wipe below unlinked an OPEN file — documented
// undefined behaviour on FatFS (volume corruption risk) and a stranded
// orphan on LittleFS.
static void _xAbort() {
  if (_xFile) _xFile.close();
  _xExpected = _xWritten = 0;
  _xTotal = _xTotalWritten = 0;
  _xActive = false;
}

// Character renderer hooks — implemented by character.cpp. Declared as
// plain functions so the linker pairs them without this file including
// the renderer's header.
void characterClose();
bool characterInit(const char* name);

// Buddy / GIF mode flags — owned by main.cpp.
extern bool buddyMode;
extern bool gifAvailable;

// Debug: hold a persona state on screen (test bridges' cmd:pose). Not part
// of the desktop protocol — exists to eyeball buddy animations during art
// work. Defined in main.cpp.
void debugPose(uint8_t s, uint32_t durMs);

// Panel blink confirming a cmd:shot capture (white = saved, red = failed)
// — visible feedback for muted devices. Defined in main.cpp.
void shotFlash(bool ok);

// Which channel the line being dispatched arrived on — set by dataPoll()
// before it feeds USB or BLE bytes into _applyJson. Replies go back to the
// same channel: BLE acks over NUS (mirrored to Serial in verbose builds),
// USB acks over Serial UNCONDITIONALLY. Routing USB acks through VWRITE
// was a bug: a non-verbose build swallowed every ack, so the device
// processed tools/usb_xfer_send.py commands but the script timed out
// waiting on silence.
static bool _xFromUsb = false;
void xferSetSource(bool fromUsb) { _xFromUsb = fromUsb; }

static void _xReply(const char* b, int len) {
  if (len <= 0) return;   // encoding error upstream — never write garbage
  if (_xFromUsb) {
    // Boot sets setTxTimeoutMs(0) so stray logs never block when no host
    // is attached — but 0 also means "drop bytes when the TX ring is
    // full". A dropped ack deadlocks the no-retry xfer protocol (host
    // waits for the ack, we wait for the next command), so block briefly
    // for protocol replies: answering a USB command implies a host is
    // attached and draining.
    Serial.setTxTimeoutMs(50);
    Serial.write((const uint8_t*)b, len);
    Serial.setTxTimeoutMs(0);
  } else {
    VWRITE(b, len);
    bleWrite((const uint8_t*)b, len);
  }
}

static void _xAck(const char* what, bool ok, uint32_t n = 0) {
  char b[64];
  int len = snprintf(b, sizeof(b),
    "{\"ack\":\"%s\",\"ok\":%s,\"n\":%lu}\n",
    what, ok ? "true" : "false", (unsigned long)n);
  _xReply(b, len);
}

// Accept only a plain single-path-segment name from the (untrusted) BLE/USB
// peer: [A-Za-z0-9._-], non-empty, not "." / "..", short enough to fit our
// fixed path buffers. Blocks path traversal ("../etc"), absolute paths and
// separators — without this, `name`/`path` flow straight into snprintf'd
// filesystem paths. Char packs are flat (no subdirs), so one segment is enough.
static bool _xSafeName(const char* s, size_t maxLen) {
  if (!s || !*s) return false;
  size_t n = 0;
  for (const char* c = s; *c; c++, n++) {
    char ch = *c;
    bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-';
    if (!ok) return false;
  }
  if (n >= maxLen) return false;
  if (strcmp(s, ".") == 0 || strcmp(s, "..") == 0) return false;
  return true;
}

// Remove a directory's contents (one level deep — char packs don't nest).
// One removal per directory open: deleting entries while iterating the
// same open handle is unspecified on the VFS layer and skips entries on
// LittleFS — a partial wipe leaked old pack files alongside the new pack.
// Packs hold a handful of files, so re-opening per entry is cheap.
// Returns total bytes reclaimed.
static uint32_t _xWipeDir(const char* dir) {
  fs::FS& fs = storageFS();
  uint32_t freed = 0;
  for (;;) {
    File d = fs.open(dir);
    if (!d || !d.isDirectory()) { fs.mkdir(dir); return freed; }
    File f = d.openNextFile();
    if (!f) { d.close(); break; }
    char p[96];
    snprintf(p, sizeof(p), "%s/%s", dir, f.name());
    uint32_t sz = f.size();
    f.close();
    d.close();
    if (!fs.remove(p)) {
      Serial.printf("[xfer] wipe: remove failed %s\n", p);
      return freed;   // bail rather than spin on an unremovable entry
    }
    freed += sz;
  }
  return freed;
}

// Wipe everything under /characters/ — only one pack lives on the device
// at a time, so installing a new pack with a different name would
// otherwise leak the old one's bytes. Same one-removal-per-open pattern
// as _xWipeDir: rmdir invalidates the open directory iterator, so re-scan
// from the top after each removed pack.
static uint32_t _xWipeAllChars() {
  fs::FS& fs = storageFS();
  uint32_t freed = 0;
  for (;;) {
    File root = fs.open("/characters");
    if (!root || !root.isDirectory()) { fs.mkdir("/characters"); return freed; }
    char p[80];
    p[0] = 0;
    File sub = root.openNextFile();
    while (sub) {
      if (sub.isDirectory()) {
        snprintf(p, sizeof(p), "/characters/%s", sub.name());
        sub.close();
        break;
      }
      sub.close();
      sub = root.openNextFile();
    }
    root.close();
    if (!p[0]) break;          // no pack directories left
    freed += _xWipeDir(p);
    if (!fs.rmdir(p)) {
      Serial.printf("[xfer] wipe: rmdir failed %s\n", p);
      break;                   // bail rather than spin on an unremovable dir
    }
  }
  return freed;
}

bool xferCommand(JsonDocument& doc) {
  const char* cmd = doc["cmd"];
  if (!cmd) return false;

  if (strcmp(cmd, "pose") == 0) {
    // Debug-only: {"cmd":"pose","s":"heart","ms":6000} — s by name or 0-6.
    static const char* const NAMES[] = {
      "sleep", "idle", "busy", "attention", "celebrate", "dizzy", "heart"
    };
    int s = -1;
    if (doc["s"].is<const char*>()) {
      const char* n = doc["s"];
      for (int i = 0; i < 7; i++)
        if (strcmp(n, NAMES[i]) == 0) { s = i; break; }
    } else if (doc["s"].is<int>()) {
      s = doc["s"];
    }
    bool ok = (s >= 0 && s <= 6);
    if (ok) debugPose((uint8_t)s, doc["ms"] | 5000);
    _xAck("pose", ok);
    return true;
  }

  if (strcmp(cmd, "shot") == 0) {
    // Debug-only: {"cmd":"shot"} — save the current frame as a BMP onto
    // SD/LittleFS (same writer as the BOOT-hold binding; screenshot.h).
    // Ack carries the saved path so the host knows which file just
    // appeared on the card.
    char path[40];
    bool ok = screenshotSave(path, sizeof(path));
    shotFlash(ok);
    char b[96];
    int len = snprintf(b, sizeof(b),
      "{\"ack\":\"shot\",\"ok\":%s,\"n\":0,\"path\":\"%s\"}\n",
      ok ? "true" : "false", path);
    _xReply(b, len);
    return true;
  }

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
    // 400 B covers the absolute worst case (~366 B with max-length pet +
    // owner names and 12-digit fs sizes). The clamp below is belt-and-
    // braces: snprintf returns the WOULD-HAVE-BEEN length, and passing
    // that to _xReply unclamped used to read past the buffer.
    char b[400];
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
    if (len >= (int)sizeof(b)) len = sizeof(b) - 1;
    _xReply(b, len);
    return true;
  }

  if (strcmp(cmd, "char_begin") == 0) {
    // A fresh char_begin while a previous transfer is still open means the
    // host restarted mid-push — close the stale file handle BEFORE any
    // wipe below touches it.
    _xAbort();
    const char* name = doc["name"] | "pet";
    _xTotal = doc["total"] | 0;

    // Validate the pack name BEFORE wiping anything — a bad/hostile name must
    // not cost the currently-installed pack or escape /characters/.
    if (!_xSafeName(name, sizeof(_xCharName))) {
      _xAck("char_begin", false);
      return true;
    }

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
      _xReply(b, len);
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
  if (!_xActive) {
    // A transfer command with no transfer open means the two sides are
    // desynced (device rebooted, or char_begin failed while the host
    // believes it succeeded). Nack explicitly — the strictly ack-paced
    // host would otherwise hang in silence waiting for a reply.
    if (strcmp(cmd, "file") == 0 || strcmp(cmd, "chunk") == 0 ||
        strcmp(cmd, "file_end") == 0 || strcmp(cmd, "char_end") == 0) {
      _xAck(cmd, false);
      return true;
    }
    return strcmp(cmd, "permission") != 0;
  }

  if (strcmp(cmd, "file") == 0) {
    const char* path = doc["path"];
    _xExpected = doc["size"] | 0;
    _xWritten = 0;
    if (!path) { _xAck("file", false); return true; }
    // Same single-segment guard as the pack name: no "..", no separators, so
    // the file can't escape this pack's directory.
    if (!_xSafeName(path, 64)) { _xAck("file", false); return true; }
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

bool     xferActive()   { return _xActive; }
uint32_t xferProgress() { return _xTotalWritten; }
uint32_t xferTotal()    { return _xTotal; }

// Abort an interrupted transfer — called by dataPoll() when the liveness
// window expires mid-push (host crashed, BLE drop, USB unplug). Closes the
// half-written file and unpins the progress screen; before this, _xActive
// stayed true forever and the device sat frozen on "installing" until the
// host happened to reconnect.
void xferAbort() {
  if (!_xActive) return;
  Serial.println("[xfer] link lost mid-transfer, aborting");
  _xAbort();
}

// Wipe /characters/ — backs the "delete char" reset path. Abort first: a
// wipe triggered mid-transfer (menu Reset, factory reset) must not unlink
// the file _xFile still holds open.
void xferDeleteAll() {
  _xAbort();
  characterClose();
  _xWipeAllChars();
  gifAvailable = false;
  buddyMode = true;
  // Reset species sentinel so future load doesn't try GIF mode.
  speciesIdxSave(0);
}
