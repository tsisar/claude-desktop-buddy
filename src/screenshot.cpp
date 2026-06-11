#include "screenshot.h"
#include "hal/display.h"
#include "hal/storage.h"
#include "hal/power.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <esp_task_wdt.h>

// Shared render surface, defined in main.cpp (same extern pattern as
// clock.cpp / character.cpp).
extern Surface gfx;

// Little-endian field writers for the BMP header.
static void put16(uint8_t* p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t* p, uint32_t v) {
  p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

bool screenshotSave(char* pathOut, size_t pathLen) {
  if (pathOut && pathLen) pathOut[0] = '\0';
  Arduino_Canvas* cv = gfx.raw();
  if (!cv) return false;
  const uint16_t* fb = cv->getFramebuffer();
  if (!fb) return false;
  if (!storageInit()) {                      // no-op once a backend is up
    Serial.println("[shot] no storage");
    return false;
  }
  fs::FS& fs = storageFS();
  if (!fs.exists("/screenshots")) fs.mkdir("/screenshots");

  // Next free shot-NNNN.bmp. `next` persists across calls so a session of
  // repeated shots probes each name once, not O(n) per shot.
  static int next = 1;
  char path[40];
  for (;; next++) {
    if (next > 9999) { Serial.println("[shot] dir full"); return false; }
    snprintf(path, sizeof(path), "/screenshots/shot-%04d.bmp", next);
    if (!fs.exists(path)) break;
  }

  const int W = gfx.width(), H = gfx.height();
  const uint32_t rowBytes = ((uint32_t)W * 3 + 3) & ~3u;   // 4-byte aligned
  const uint32_t dataSize = rowBytes * (uint32_t)H;

  uint8_t* row = (uint8_t*)malloc(rowBytes);
  if (!row) return false;
  memset(row, 0, rowBytes);                  // padding bytes stay zero

  File f = fs.open(path, FILE_WRITE);
  if (!f) { free(row); Serial.printf("[shot] open failed %s\n", path); return false; }

  // 54-byte BITMAPFILEHEADER + BITMAPINFOHEADER, 24bpp, bottom-up rows —
  // the most universally decodable BMP flavour.
  uint8_t hdr[54] = {0};
  hdr[0] = 'B'; hdr[1] = 'M';
  put32(hdr + 2,  54 + dataSize);            // file size
  put32(hdr + 10, 54);                       // pixel data offset
  put32(hdr + 14, 40);                       // BITMAPINFOHEADER size
  put32(hdr + 18, (uint32_t)W);
  put32(hdr + 22, (uint32_t)H);              // positive ⇒ bottom-up
  put16(hdr + 26, 1);                        // planes
  put16(hdr + 28, 24);                       // bpp
  put32(hdr + 34, dataSize);
  f.write(hdr, sizeof(hdr));

  // RGB565 → BGR888 with bit replication so full-scale stays full-scale
  // (0x1F → 0xFF, not 0xF8). Rows written bottom-up per the header.
  for (int y = H - 1; y >= 0; y--) {
    const uint16_t* src = fb + (uint32_t)y * W;
    uint8_t* d = row;
    for (int x = 0; x < W; x++) {
      uint16_t c = src[x];
      uint8_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
      *d++ = (uint8_t)((b << 3) | (b >> 2));
      *d++ = (uint8_t)((g << 2) | (g >> 4));
      *d++ = (uint8_t)((r << 3) | (r >> 2));
    }
    if (f.write(row, rowBytes) != rowBytes) {
      f.close(); free(row);
      // Don't leave the truncated BMP behind: each stub is ~0.5 MB, the
      // next probe would skip past it (stranding it forever), and on the
      // 3.5 MB LittleFS fallback a handful of failed shots fills the
      // partition shared with the character pack.
      fs.remove(path);
      Serial.printf("[shot] write failed %s\n", path);
      return false;
    }
    if ((y & 31) == 0) {
      // A slow card / LittleFS erase storm can stretch this loop past the
      // watchdog windows — neither the AXP hardware WDT (8 s power-cut)
      // nor the loop-task WDT (5 s panic) is fed while loop() is blocked
      // in here. Feed both and yield so the IDLE task runs.
      powerFeedWatchdog();
      esp_task_wdt_reset();
      yield();
    }
  }

  f.close();
  free(row);
  Serial.printf("[shot] saved %s (%dx%d, %s)\n", path, W, H, storageBackendName());
  if (pathOut && pathLen) strlcpy(pathOut, path, pathLen);
  next++;
  return true;
}
