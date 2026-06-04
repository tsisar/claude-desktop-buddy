// Pixel-art SVG animation player — see svg_anim.h for the model (one file
// per frame, fixed interval, any frame count).
//
// The parser is a single streaming pass over each file (512-byte read
// buffer, no DOM): tags are scanned for <path>/<rect>/viewBox. A path's
// "d" attribute — the only thing that can be kilobytes long — is decoded
// straight from the stream into rect records, one per M...Z subpath.
// Subpaths in this dialect are always axis-aligned rectangles, so the
// bounding box of a subpath IS the rectangle; tracking min/max while the
// numbers stream by is all the geometry needed. <rect> elements carry
// their geometry as plain attributes.
//
// Memory model: three flat pools (frames → runs → rects) built with
// std::vector during parse. A 9-frame sequence of ~200-rect artboard
// files lands around 15 KB — well below one decoded GIF frame.

#include "svg_anim.h"
#include "hal/display.h"
#include "hal/storage.h"
#include <Arduino.h>
#include <FS.h>
#include <math.h>
#include <string.h>
#include <vector>

extern Surface gfx;

// ── tables ────────────────────────────────────────────────────────────────
struct SvgRect { int16_t x, y, w, h; };                       // SVG units
struct SvgRun  { uint32_t rectStart; uint16_t rectCount; uint16_t color; };
struct SvgFrame{ uint32_t runStart;  uint16_t runCount; };

static std::vector<SvgRect>  rects;
static std::vector<SvgRun>   runs;
static std::vector<SvgFrame> frames;
static bool  loaded  = false;
static bool  seqOpen = false;           // between SeqBegin and SeqEnd

static float vbX = 0, vbY = 0, vbW = 100, vbH = 100;   // content box

// Placement (set by svgAnimPlace; defaults to the full canvas at first draw)
static bool  placed = false;
static float scale  = 1.0f;
static int   ox = 0, oy = 0;            // canvas px of content-box origin
static int   clearW = 0, clearH = 0;    // scaled content-box extent

static uint32_t cycleMs    = 2000;      // frameDelay × frame count
static uint32_t cycleStart = 0;         // millis() anchor, set at load

// ── buffered file reader ──────────────────────────────────────────────────
struct Reader {
  File*   f;
  uint8_t buf[512];
  int     len = 0, pos = 0;
  int next() {
    if (pos >= len) {
      len = f->read(buf, sizeof(buf));
      pos = 0;
      if (len <= 0) return -1;
    }
    return buf[pos++];
  }
};

static uint16_t hexToRgb565(const char* s, uint16_t fallback) {
  if (!s || *s != '#') return fallback;
  uint32_t v = strtoul(s + 1, nullptr, 16);
  return (uint16_t)(((v >> 19) & 0x1F) << 11
                  | ((v >> 10) & 0x3F) << 5
                  | ((v >> 3)  & 0x1F));
}

// ── path data ("d" attribute) ─────────────────────────────────────────────
// Decodes M/L/H/V (+ lowercase relatives) and Z, emitting one rect per
// subpath bounding box. Stops at the closing quote. Returns rects added.
static int parsePathData(Reader& rd, char quote) {
  int   added = 0;
  float cx = 0, cy = 0;                 // current point
  float sx = 0, sy = 0;                 // subpath start (for Z)
  float mnX = 0, mnY = 0, mxX = 0, mxY = 0;
  bool  open = false;                   // inside a subpath with ≥1 point
  char  cmd = 0;
  int   c = rd.next();

  auto track = [&]() {
    if (!open) { mnX = mxX = cx; mnY = mxY = cy; open = true; return; }
    if (cx < mnX) mnX = cx; if (cx > mxX) mxX = cx;
    if (cy < mnY) mnY = cy; if (cy > mxY) mxY = cy;
  };
  auto emit = [&]() {
    if (!open) return;
    int16_t w = (int16_t)lroundf(mxX - mnX);
    int16_t h = (int16_t)lroundf(mxY - mnY);
    if (w > 0 && h > 0) {
      rects.push_back({ (int16_t)lroundf(mnX), (int16_t)lroundf(mnY), w, h });
      added++;
    }
    open = false;
  };
  // Reads one number starting at c; leaves c on the first non-number char.
  auto number = [&](float& out) -> bool {
    while (c == ' ' || c == ',' || c == '\t' || c == '\n' || c == '\r') c = rd.next();
    char nb[16]; int n = 0;
    while (c >= 0 && n < 15 &&
           ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.')) {
      nb[n++] = (char)c;
      c = rd.next();
    }
    if (!n) return false;
    nb[n] = 0;
    out = strtof(nb, nullptr);
    return true;
  };

  while (c >= 0 && c != quote) {
    if (c == ' ' || c == ',' || c == '\t' || c == '\n' || c == '\r') { c = rd.next(); continue; }
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
      cmd = (char)c;
      c = rd.next();
      if (cmd == 'Z' || cmd == 'z') { emit(); cx = sx; cy = sy; }
      continue;
    }
    // A number: consume per the active command (commands repeat implicitly).
    float a, b;
    switch (cmd) {
      case 'M': case 'L':
        if (!number(a) || !number(b)) return added;
        if (cmd == 'M') { emit(); sx = a; sy = b; cmd = 'L'; }
        cx = a; cy = b; track(); break;
      case 'm': case 'l':
        if (!number(a) || !number(b)) return added;
        if (cmd == 'm') { emit(); cx += a; cy += b; sx = cx; sy = cy; cmd = 'l'; }
        else            { cx += a; cy += b; }
        track(); break;
      case 'H': if (!number(a)) return added; cx  = a; track(); break;
      case 'h': if (!number(a)) return added; cx += a; track(); break;
      case 'V': if (!number(a)) return added; cy  = a; track(); break;
      case 'v': if (!number(a)) return added; cy += a; track(); break;
      default:  c = rd.next(); break;   // unsupported command — skip char
    }
  }
  emit();                               // unterminated subpath at quote/EOF
  return added;
}

// ── tag attribute scan ────────────────────────────────────────────────────
struct TagAttrs {
  char fill[12];
  char opacity[8];
  char viewBox[40];
  char x[16], y[16], w[16], h[16];      // <rect> geometry
};

// After the tag name: walks name="value" pairs until '>' (returns 1) or
// '/>' (returns 2), -1 on EOF. For <path d=...> the value is streamed into
// parsePathData; everything else is captured into the TagAttrs buffers.
static int parseAttrs(Reader& rd, bool isPath, TagAttrs* out) {
  if (out) {
    out->fill[0] = 0; out->opacity[0] = 0; out->viewBox[0] = 0;
    out->x[0] = 0; out->y[0] = 0; out->w[0] = 0; out->h[0] = 0;
  }
  int c = rd.next();
  for (;;) {
    while (c == ' ' || c == '\t' || c == '\n' || c == '\r') c = rd.next();
    if (c < 0)   return -1;
    if (c == '>') return 1;
    if (c == '/') { c = rd.next(); if (c == '>') return 2; continue; }

    char an[16]; int n = 0;
    while (c >= 0 && c != '=' && c != ' ' && c != '>' && c != '/') {
      if (n < 15) an[n++] = (char)c;
      c = rd.next();
    }
    an[n] = 0;
    if (c != '=') continue;             // valueless attr — resume scan
    c = rd.next();
    if (c != '"' && c != '\'') continue;
    char quote = (char)c;

    if (isPath && strcmp(an, "d") == 0) {
      parsePathData(rd, quote);         // consumes through closing quote
      c = rd.next();
      continue;
    }
    char av[40]; int vn = 0;
    c = rd.next();
    while (c >= 0 && c != quote) {
      if (vn < 39) av[vn++] = (char)c;
      c = rd.next();
    }
    av[vn] = 0;
    c = rd.next();
    if (!out) continue;
    if      (strcmp(an, "fill")         == 0) strlcpy(out->fill,    av, sizeof(out->fill));
    else if (strcmp(an, "fill-opacity") == 0) strlcpy(out->opacity, av, sizeof(out->opacity));
    else if (strcmp(an, "viewBox")      == 0) strlcpy(out->viewBox, av, sizeof(out->viewBox));
    else if (strcmp(an, "x")            == 0) strlcpy(out->x,       av, sizeof(out->x));
    else if (strcmp(an, "y")            == 0) strlcpy(out->y,       av, sizeof(out->y));
    else if (strcmp(an, "width")        == 0) strlcpy(out->w,       av, sizeof(out->w));
    else if (strcmp(an, "height")       == 0) strlcpy(out->h,       av, sizeof(out->h));
  }
}

static void skipTag(Reader& rd) {       // skip to '>' (quote-aware)
  int c, q = 0;
  while ((c = rd.next()) >= 0) {
    if (q)                          { if (c == q) q = 0; }
    else if (c == '"' || c == '\'') q = c;
    else if (c == '>')              return;
  }
}

// ── file parse ────────────────────────────────────────────────────────────
// The whole file lands in frames.back(). <g> nesting is only skipped over;
// fill-opacity="0" shapes are dropped (an editor's hidden siblings aren't
// part of this frame). Both <path> (rect-union d) and flat <rect> shapes
// are accepted.
static bool parseSvgFile(const char* path) {
  File f = storageFS().open(path, "r");
  if (!f) {
    Serial.printf("[svg] open failed: %s\n", path);
    return false;
  }
  Reader rd{ &f };

  int  c;
  char nameBuf[8];
  TagAttrs at;
  while ((c = rd.next()) >= 0) {
    if (c != '<') continue;
    c = rd.next();
    if (c == '/') { while ((c = rd.next()) >= 0 && c != '>') {} continue; }
    if (c == '?' || c == '!') { skipTag(rd); continue; }

    int n = 0;
    while (c >= 0 && c != ' ' && c != '>' && c != '/' &&
           c != '\t' && c != '\n' && c != '\r') {
      if (n < 7) nameBuf[n++] = (char)c;
      c = rd.next();
    }
    nameBuf[n] = 0;

    if (strcmp(nameBuf, "svg") == 0 && c == ' ') {
      parseAttrs(rd, false, &at);
      if (at.viewBox[0]) sscanf(at.viewBox, "%f %f %f %f", &vbX, &vbY, &vbW, &vbH);
    } else if (strcmp(nameBuf, "path") == 0 || strcmp(nameBuf, "rect") == 0) {
      bool isRect = (nameBuf[0] == 'r');
      uint32_t rectStart = (uint32_t)rects.size();
      int r = (c == '>') ? 1 : parseAttrs(rd, !isRect, &at);
      if (r < 0) break;
      if (isRect) {
        // <rect x y width height> — one rect, geometry straight from attrs.
        float rx = strtof(at.x, nullptr), ry = strtof(at.y, nullptr);
        float rw = strtof(at.w, nullptr), rh = strtof(at.h, nullptr);
        int16_t w = (int16_t)lroundf(rw), h = (int16_t)lroundf(rh);
        if (w > 0 && h > 0)
          rects.push_back({ (int16_t)lroundf(rx), (int16_t)lroundf(ry), w, h });
      }
      uint16_t cnt = (uint16_t)(rects.size() - rectStart);
      bool hidden = at.opacity[0] && strtof(at.opacity, nullptr) == 0.0f;
      if (cnt && !hidden && strcmp(at.fill, "none") != 0) {
        uint16_t col = hexToRgb565(at.fill, 0xFFFF);
        // Coalesce with the previous run when the color matches and the
        // rects are contiguous — flat artboard exports emit one <rect>
        // per pixel cell, which would otherwise cost a run record each.
        if (frames.back().runCount && !runs.empty() &&
            runs.back().color == col &&
            runs.back().rectStart + runs.back().rectCount == rectStart) {
          runs.back().rectCount += cnt;
        } else {
          runs.push_back({ rectStart, cnt, col });
          frames.back().runCount++;
        }
      } else if (cnt) {
        rects.resize(rectStart);        // hidden or fill="none" — drop it
      }
    } else if (c != '>') {
      skipTag(rd);
    }
  }
  f.close();
  return true;
}

// ── load / free ───────────────────────────────────────────────────────────
static void freeTables() {
  rects.clear();  rects.shrink_to_fit();
  runs.clear();   runs.shrink_to_fit();
  frames.clear(); frames.shrink_to_fit();
}

void svgAnimClose() {
  freeTables();
  loaded  = false;
  seqOpen = false;
  placed  = false;
}

bool svgAnimLoaded() { return loaded; }

// Replace the file's viewBox with the tight bounding box of the actual
// content across ALL frames. Artboard exports park a small sprite in the
// middle of a huge blank canvas; scaling to the canvas would render the
// buddy tiny. The box is global, so frames stay aligned to each other and
// any authored motion survives.
static void fitViewBoxToContent() {
  if (rects.empty()) return;
  int16_t mnX = rects[0].x, mnY = rects[0].y;
  int16_t mxX = mnX, mxY = mnY;
  for (const SvgRect& r : rects) {
    if (r.x < mnX) mnX = r.x;
    if (r.y < mnY) mnY = r.y;
    if (r.x + r.w > mxX) mxX = r.x + r.w;
    if (r.y + r.h > mxY) mxY = r.y + r.h;
  }
  vbX = mnX; vbY = mnY;
  vbW = mxX - mnX; vbH = mxY - mnY;
}

void svgAnimSeqBegin() {
  svgAnimClose();
  seqOpen = true;
}

bool svgAnimSeqAddFrame(const char* path) {
  if (!seqOpen) return false;
  uint32_t rectStart = (uint32_t)rects.size();
  frames.push_back({ (uint32_t)runs.size(), 0 });
  if (!parseSvgFile(path)) {
    frames.pop_back();
    return false;
  }
  if (frames.back().runCount == 0) {
    // Nothing visible in this file — keep it anyway as an intentional
    // blank frame (useful for blink-style animations), but flag it.
    Serial.printf("[svg] frame %u (%s) is empty\n",
                  (unsigned)(frames.size() - 1), path);
    rects.resize(rectStart);
  }
  return true;
}

bool svgAnimSeqEnd(uint16_t frameDelayMs) {
  if (!seqOpen) return false;
  seqOpen = false;
  if (frames.empty() || rects.empty()) {
    Serial.println("[svg] sequence has no drawable frames");
    freeTables();
    return false;
  }
  fitViewBoxToContent();
  if (vbW <= 0 || vbH <= 0) { vbW = 100; vbH = 100; }

  if (!frameDelayMs) frameDelayMs = 200;
  cycleMs = (uint32_t)frameDelayMs * frames.size();
  loaded = true;
  cycleStart = millis();
  Serial.printf("[svg] sequence: %u frames, %u rects (%u B) cycle=%lums heap=%u\n",
                (unsigned)frames.size(), (unsigned)rects.size(),
                (unsigned)(rects.size() * sizeof(SvgRect) + runs.size() * sizeof(SvgRun)
                         + frames.size() * sizeof(SvgFrame)),
                (unsigned long)cycleMs, ESP.getFreeHeap());
  return true;
}

// ── playback ──────────────────────────────────────────────────────────────
uint32_t svgAnimNextEventMs() {
  if (!loaded || frames.size() < 2) return 200;   // static image: idle pace
  uint32_t t = (uint32_t)((millis() - cycleStart) % cycleMs);
  // Frame idx = floor(t*n/T) flips at the smallest t' with t'*n/T ≥ idx+1,
  // i.e. t' = ceil((idx+1)*T/n).
  uint32_t n    = (uint32_t)frames.size();
  uint32_t idx  = (uint64_t)t * n / cycleMs;
  uint64_t next = ((uint64_t)(idx + 1) * cycleMs + n - 1) / n;
  uint32_t dt   = (next > t) ? (uint32_t)(next - t) : 1;
  return dt;
}

// ── placement & draw ──────────────────────────────────────────────────────
void svgAnimPlace(int x, int y, int w, int h) {
  if (w <= 0 || h <= 0) return;
  float s = (float)w / vbW;
  float sy = (float)h / vbH;
  if (sy < s) s = sy;
  scale = s;
  clearW = (int)(vbW * s + 0.5f);
  clearH = (int)(vbH * s + 0.5f);
  ox = x + (w - clearW) / 2;
  oy = y + (h - clearH) / 2;
  placed = true;
}

void svgAnimTick(uint16_t bgColor) {
  if (!loaded) return;
  if (!placed) svgAnimPlace(0, 0, gfx.width(), gfx.height());

  uint32_t t   = (uint32_t)((millis() - cycleStart) % cycleMs);
  uint32_t idx = (uint64_t)t * frames.size() / cycleMs;
  if (idx >= frames.size()) idx = frames.size() - 1;   // t == cycleMs-ε guard

  gfx.fillRect(ox, oy, clearW, clearH, bgColor);

  const SvgFrame& fr = frames[idx];
  for (uint16_t ri = 0; ri < fr.runCount; ri++) {
    const SvgRun& run = runs[fr.runStart + ri];
    for (uint16_t i = 0; i < run.rectCount; i++) {
      const SvgRect& r = rects[run.rectStart + i];
      // Round both edges from SVG units so adjacent cells share the
      // exact pixel boundary — no seams at non-integer scales.
      int x1 = ox + (int)lroundf((r.x - vbX) * scale);
      int y1 = oy + (int)lroundf((r.y - vbY) * scale);
      int x2 = ox + (int)lroundf((r.x + r.w - vbX) * scale);
      int y2 = oy + (int)lroundf((r.y + r.h - vbY) * scale);
      if (x2 > x1 && y2 > y1)
        gfx.fillRect(x1, y1, x2 - x1, y2 - y1, run.color);
    }
  }
}
