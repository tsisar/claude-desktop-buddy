// Reads a GIF character pack from storage (storageFS(), so SD or LittleFS
// as picked by hal/storage.h), parses the manifest, opens one GIF per
// persona state, and draws it to the global Surface (gfx) using
// AnimatedGIF. Per-pixel writes in the GIF draw callback go through
// Surface::drawPixel().
//
// Text mode (manifest "mode":"text" with frame strings instead of GIFs)
// stays supported — same code path, rendered through Surface.
//
// SVG mode: a state entry ending in ".svg" is played through svg_anim
// (pixel-art SVG made of axis-aligned rects) instead of AnimatedGIF.
// Detection is per state, so a pack can mix GIF and SVG states freely.
//   "idle": ["idle_0.svg", "idle_1.svg", ...]
// One file per frame, stepped at a fixed interval and looped — however
// many files are listed is how many frames play (a single file is a
// static image). NOTE this differs from GIF arrays, which are rotating
// variants, not frames. Optional top-level manifest keys:
//   "svgDelay" — ms each frame is shown (default 200)
//   "svgWidth"/"svgHeight" — max rendered size in panel px (aspect is
//                   always kept; omitted = fill the upper home area)

#include "character.h"
#include "svg_anim.h"
#include "hal/display.h"
#include "hal/storage.h"
#include "debug.h"
#include <Arduino.h>
#include <FS.h>
#include <AnimatedGIF.h>
#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>   // Arduino_Canvas — direct row blit below
#include <string.h>

extern Surface gfx;
extern int LCD_WIDTH;
extern int LCD_HEIGHT;
// Bring in the buddy_common.h symbols so HUD position math agrees.
// HUD_TOP from main.cpp isn't visible here, so reproduce its
// derivation locally — both files need to match if either moves the HUD.
static int homeUpperBottom() { return gfx.height() - 130; }

static const char* STATE_NAMES[] = {
  "sleep", "idle", "busy", "attention", "celebrate", "dizzy", "heart"
};
static const uint8_t N_STATES = 7;

// Text mode: manifest has "mode":"text", states contain {frames:[...], delay:N}.
struct TextState {
  char     frames[8][20];
  uint8_t  nFrames;
  uint16_t delayMs;
};
static TextState textStates[N_STATES];
static bool      textMode = false;
static uint8_t   textFrame = 0;
static uint32_t  textNext  = 0;

static bool    loaded = false;
static Palette pal = { 0xC2A6, 0x0000, 0xFFFF, 0x8410, 0x0000 };
static char    basePath[48];
// 64 entries × 48 chars: SVG frame sequences list one file per FRAME (a
// 10-frame attention alone is 10 entries) and artboard exports have long
// descriptive names ("developer-pair-programming-scene.svg" is 37 chars —
// the old 32-char cells truncated it into a file that never opens).
static const uint8_t MAX_GIFS = 64;
static char    gifPaths[MAX_GIFS][48];
static uint8_t stateStart[N_STATES];
static uint8_t stateCount[N_STATES];
static uint8_t stateRot[N_STATES];
static uint8_t gifTotal = 0;
static uint8_t curState = 0xFF;

static AnimatedGIF gif;
static File        gifFile;
static int         gifX = 0, gifY = 0, gifW = 0, gifH = 0;
static uint32_t    nextFrameAt = 0;
static uint32_t    animPauseUntil = 0;
static uint32_t    variantStartedMs = 0;
static const uint32_t VARIANT_DWELL_MS = 5000;
static const uint32_t ANIM_PAUSE_MS    = 800;
static bool        gifOpen = false;
static bool        svgMode = false;     // current state plays through svg_anim
static int16_t     svgW = 0, svgH = 0;  // manifest size cap, 0 = unset
static uint16_t    svgDelay = 200;      // ms per frame for svg sequences

static bool isSvgFile(const char* p) {
  const char* e = strrchr(p, '.');
  return e && strcasecmp(e, ".svg") == 0;
}

static uint16_t parseHexColor(const char* s, uint16_t fallback) {
  if (!s) return fallback;
  if (*s == '#') s++;
  uint32_t v = strtoul(s, nullptr, 16);
  return (uint16_t)(((v >> 19) & 0x1F) << 11
                  | ((v >> 10) & 0x3F) << 5
                  | ((v >> 3)  & 0x1F));
}

// Place the GIF body centred in the upper home area (above the HUD).
// Source frames are typically 96px-wide / variable height.
static void gifPlace() {
  gifX = (gfx.width() - gifW) / 2;
  int upper = homeUpperBottom();
  gifY = (upper - gifH) / 2;
  if (gifY < 4) gifY = 4;
}

// ── AnimatedGIF file callbacks (storageFS) ────────────────────────────────
static void* gifOpenCb(const char* fname, int32_t* pSize) {
  gifFile = storageFS().open(fname, "r");
  if (!gifFile) return nullptr;
  *pSize = gifFile.size();
  return (void*)&gifFile;
}
static void gifCloseCb(void* handle) {
  File* f = (File*)handle;
  if (f) f->close();
}
static int32_t gifReadCb(GIFFILE* pFile, uint8_t* pBuf, int32_t iLen) {
  File* f = (File*)pFile->fHandle;
  int32_t n = f->read(pBuf, iLen);
  pFile->iPos = f->position();
  return n;
}
static int32_t gifSeekCb(GIFFILE* pFile, int32_t iPosition) {
  File* f = (File*)pFile->fHandle;
  f->seek(iPosition);
  pFile->iPos = (int32_t)f->position();
  return pFile->iPos;
}

// ── Draw callback: one scanline through Surface::drawPixel() ──────────────
//
// AMOLED canvas is 368×448 — much bigger than the M5 panel — so the GIF
// (~96px wide) sits centred with plenty of margin. Transparent pixels
// fall back to the pack's bg colour so each frame fully paints its rect
// instead of leaking the previous frame through.
static void gifDrawCb(GIFDRAW* d) {
  uint16_t* pal16 = d->pPalette;
  uint8_t*  src   = d->pPixels;
  uint8_t   t     = d->ucTransparent;
  bool      hasT  = d->ucHasTransparency;
  int       srcY  = d->iY + d->y;

  int y = gifY + srcY;
  if (y < 0 || y >= gfx.height()) return;
  int x0 = gifX + d->iX;
  int w  = d->iWidth;
  if (x0 < 0) { src -= x0; w += x0; x0 = 0; }
  if (x0 + w > gfx.width()) w = gfx.width() - x0;
  if (w <= 0) return;
  // Blit the row straight into the canvas framebuffer (same pattern the
  // screenshot writer uses to read it). The old per-pixel path went
  // through a virtual Arduino_Canvas::drawPixel per pixel — call +
  // bounds/rotation overhead on >100K pixels per frame for large packs,
  // dwarfing the LZW decode. Clipping already happened above, so the
  // output is bit-identical.
  Arduino_Canvas* cv = gfx.raw();
  uint16_t* fb = cv ? cv->getFramebuffer() : nullptr;
  uint16_t bg = pal.bg;
  if (fb) {
    uint16_t* row = fb + (int32_t)y * cv->width() + x0;
    for (int i = 0; i < w; i++) {
      uint8_t idx = src[i];
      row[i] = (hasT && idx == t) ? bg : pal16[idx];
    }
  } else {
    for (int i = 0; i < w; i++) {
      uint8_t idx = src[i];
      gfx.drawPixel(x0 + i, y, (hasT && idx == t) ? bg : pal16[idx]);
    }
  }
}

// ── Public API ────────────────────────────────────────────────────────────
bool characterInit(const char* name) {
  // storageInit() already ran in setup(); we just hop on the
  // current backend. If it's STORAGE_NONE there's nothing to load.
  if (storageBackend() == STORAGE_NONE) {
    Serial.println("[char] no storage backend");
    return false;
  }

  static char scanned[24];
  if (!name) {
    File d = storageFS().open("/characters");
    if (d && d.isDirectory()) {
      File e = d.openNextFile();
      while (e) {
        if (e.isDirectory()) {
          const char* n = strrchr(e.name(), '/');
          strncpy(scanned, n ? n + 1 : e.name(), sizeof(scanned) - 1);
          scanned[sizeof(scanned) - 1] = 0;
          name = scanned;
          break;
        }
        e = d.openNextFile();
      }
      d.close();
    }
    if (!name) { Serial.println("[char] no characters installed"); return false; }
  }

  snprintf(basePath, sizeof(basePath), "/characters/%s", name);
  char mpath[64];
  snprintf(mpath, sizeof(mpath), "%s/manifest.json", basePath);

  File mf = storageFS().open(mpath, "r");
  if (!mf) {
    Serial.printf("[char] manifest not found: %s\n", mpath);
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, mf);
  mf.close();
  if (err) {
    Serial.printf("[char] manifest parse: %s\n", err.c_str());
    return false;
  }

  JsonObject colors = doc["colors"];
  pal.body    = parseHexColor(colors["body"],    pal.body);
  pal.bg      = parseHexColor(colors["bg"],      pal.bg);
  pal.text    = parseHexColor(colors["text"],    pal.text);
  pal.textDim = parseHexColor(colors["textDim"], pal.textDim);
  pal.ink     = parseHexColor(colors["ink"],     pal.ink);

  const char* mode = doc["mode"];
  textMode = (mode && strcmp(mode, "text") == 0);

  JsonObject states = doc["states"];

  if (textMode) {
    for (uint8_t i = 0; i < N_STATES; i++) {
      TextState& ts = textStates[i];
      ts.nFrames = 0;
      ts.delayMs = 200;
      JsonObject st = states[STATE_NAMES[i]];
      if (st.isNull()) continue;
      ts.delayMs = st["delay"] | 200;
      JsonArray fr = st["frames"];
      for (JsonVariant v : fr) {
        if (ts.nFrames >= 8) break;
        const char* s = v.as<const char*>();
        strncpy(ts.frames[ts.nFrames], s ? s : "", 19);
        ts.frames[ts.nFrames][19] = 0;
        ts.nFrames++;
      }
    }
    loaded = true;
    Serial.printf("[char] loaded '%s' (text mode)\n", name);
    return true;
  }

  gifTotal = 0;
  for (uint8_t i = 0; i < N_STATES; i++) {
    stateStart[i] = gifTotal;
    stateCount[i] = 0;
    stateRot[i]   = 0;
    JsonVariant v = states[STATE_NAMES[i]];
    if (v.is<JsonArray>()) {
      for (JsonVariant e : v.as<JsonArray>()) {
        if (gifTotal >= MAX_GIFS) break;
        const char* fn = e.as<const char*>();
        if (fn) {
          snprintf(gifPaths[gifTotal], sizeof(gifPaths[0]), "%s", fn);
          gifTotal++; stateCount[i]++;
        }
      }
    } else {
      const char* fn = v.as<const char*>();
      if (fn) {
        snprintf(gifPaths[gifTotal], sizeof(gifPaths[0]), "%s", fn);
        gifTotal++; stateCount[i] = 1;
      }
    }
  }

  svgDelay = doc["svgDelay"] | 200;
  svgW = doc["svgWidth"]  | 0;
  svgH = doc["svgHeight"] | 0;
  gif.begin(LITTLE_ENDIAN_PIXELS);
  loaded = true;
  Serial.printf("[char] loaded '%s' from %s (%u gifs)\n",
                (const char*)doc["name"], basePath, gifTotal);
  return true;
}

bool characterLoaded()          { return loaded; }

void characterClose() {
  if (gifOpen) { gif.close(); gifOpen = false; }
  if (svgMode) { svgAnimClose(); svgMode = false; }
  loaded = false;
  textMode = false;
  curState = 0xFF;
}


void characterSetState(uint8_t s) {
  if (!loaded || s >= N_STATES || s == curState) return;

  if (textMode) {
    curState = s;
    textFrame = 0;
    textNext = 0;
    return;
  }

  if (gifOpen) { gif.close(); gifOpen = false; }
  if (svgMode) { svgAnimClose(); svgMode = false; }
  animPauseUntil = 0;
  curState = s;

  if (stateCount[s] == 0) {
    Serial.printf("[char] no gif for state %d\n", s);
    return;
  }

  uint8_t idx = stateStart[s] + stateRot[s];
  char full[96];
  snprintf(full, sizeof(full), "%s/%s", basePath, gifPaths[idx]);
  if (isSvgFile(gifPaths[stateStart[s]])) {
    // Every entry of the state is one FRAME, stepped at svgDelay. One
    // entry = a static image; the array length sets the frame count.
    svgAnimSeqBegin();
    bool ok = true;
    for (uint8_t i = 0; i < stateCount[s] && ok; i++) {
      snprintf(full, sizeof(full), "%s/%s",
               basePath, gifPaths[stateStart[s] + i]);
      ok = svgAnimSeqAddFrame(full);
    }
    ok = ok && svgAnimSeqEnd(svgDelay);
    if (!ok) svgAnimClose();
    if (ok) {
      svgMode = true;
      // Same slot the GIF gets: centered in the upper home area, above
      // the HUD, with a small margin all round. A manifest svgWidth/
      // svgHeight shrinks the fit box (clamped to the area); aspect is
      // preserved either way, so one dimension acts as a max.
      int ax = 4, ay = 4;
      int aw = gfx.width() - 8, ah = homeUpperBottom() - 8;
      int bw = (svgW > 0 && svgW < aw) ? svgW : aw;
      int bh = (svgH > 0 && svgH < ah) ? svgH : ah;
      svgAnimPlace(ax + (aw - bw) / 2, ay + (ah - bh) / 2, bw, bh);
    }
    return;
  }
  if (gif.open(full, gifOpenCb, gifCloseCb, gifReadCb, gifSeekCb, gifDrawCb)) {
    gifOpen = true;
    gifW = gif.getCanvasWidth();
    gifH = gif.getCanvasHeight();
    gifPlace();
    nextFrameAt = 0;
    variantStartedMs = millis();
    VLOG("[char] %s: %dx%d @ (%d,%d) heap=%u\n",
         gifPaths[idx], gifW, gifH, gifX, gifY, ESP.getFreeHeap());
  } else {
    Serial.printf("[char] open failed: %s (err %d)\n", full, gif.getLastError());
  }
}

void characterTick() {
  if (!loaded) return;

  if (textMode) {
    TextState& ts = textStates[curState];
    if (ts.nFrames == 0) return;
    uint32_t now = millis();
    if (now < textNext) return;
    textNext = now + ts.delayMs;

    int cy = 60;
    gfx.fillRect(0, cy - 14, gfx.width(), 28, pal.bg);

    const char* line = ts.frames[textFrame];
    int len = strlen(line);
    int tw = len * 12;
    gfx.setTextColor(pal.body, pal.bg);
    gfx.setTextSize(2);
    gfx.setCursor((gfx.width() - tw) / 2, cy - 8);
    gfx.print(line);

    textFrame = (textFrame + 1) % ts.nFrames;
    return;
  }

  uint32_t now = millis();

  if (svgMode) {
    // An SVG array is FRAMES of one looping animation (not variants like
    // GIF arrays), so there's no dwell rotation — just draw.
    svgAnimTick(pal.bg);
    return;
  }

  if (!gifOpen) {
    // Between gifs in a rotation: hold the last frame, reopen on dwell end.
    if (animPauseUntil && now >= animPauseUntil) {
      animPauseUntil = 0;
      uint8_t s = curState; curState = 0xFF;
      characterSetState(s);
    }
    return;
  }
  if (now < nextFrameAt) return;

  int delayMs = 0;
  if (!gif.playFrame(false, &delayMs)) {
    // End of animation. Single-gif states freeze on the last frame.
    if (stateCount[curState] == 1) {
      gif.close();
      gifOpen = false;
      return;
    }
    // Multi-variant: loop the same GIF until the dwell window elapses, then
    // rotate. Short bufo idles get many plays instead of one flash + freeze.
    if (now - variantStartedMs < VARIANT_DWELL_MS) {
      gif.reset();
      nextFrameAt = now;
      return;
    }
    gif.close(); gifOpen = false;
    stateRot[curState] = (stateRot[curState] + 1) % stateCount[curState];
    animPauseUntil = now + ANIM_PAUSE_MS;
    return;
  }
  nextFrameAt = now + (delayMs > 0 ? delayMs : 100);
}
