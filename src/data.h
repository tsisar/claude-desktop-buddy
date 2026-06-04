#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <string.h>
#include <time.h>
#include "ble_bridge.h"
#include "hal/rtc.h"
#include "stats.h"

// AMOLED port of data.h. Same public API + TamaState layout — so any UI
// code from the M5 main.cpp ports verbatim — but the RTC sync path goes
// through hal/rtc.h instead of M5.Rtc, and the xfer.h GIF-stream hook is
// stubbed until character.cpp + xfer.h are ported (Stage 4).
//
// Header-only with file-static state: include from exactly one translation
// unit. Including twice will trip duplicate-symbol link errors.

// Transcript backlog the device retains. The home HUD only shows the last
// few; the full-screen log view scrolls the whole buffer, so keep enough for
// a useful history without blowing the static footprint (N * 92 bytes).
static const uint8_t TAMA_MAX_LINES = 16;

struct TamaState {
  uint8_t  sessionsTotal;
  uint8_t  sessionsRunning;
  uint8_t  sessionsWaiting;
  bool     recentlyCompleted;
  uint32_t tokensToday;
  uint32_t lastUpdated;
  char     msg[24];
  bool     connected;
  char     lines[TAMA_MAX_LINES][92];
  uint8_t  nLines;
  uint16_t lineGen;          // bumps when lines change — lets UI reset scroll
  char     promptId[40];     // pending permission request ID; empty = no prompt
  char     promptTool[20];
  char     promptHint[44];
};

// ---------------------------------------------------------------------------
// Three modes, checked in priority order:
//   demo   → auto-cycle fake scenarios every 8s, ignore live data
//   live   → JSON arrived in the last 30s over USB or BLE
//   asleep → no data, all zeros, "No Claude connected"
// ---------------------------------------------------------------------------

static uint32_t _lastLiveMs   = 0;
static uint32_t _lastBtByteMs = 0;   // BLE liveness — hasClient() lies; track bytes
static bool     _demoMode     = false;
static uint8_t  _demoIdx      = 0;
static uint32_t _demoNext     = 0;

struct _Fake { const char* n; uint8_t t,r,w; bool c; uint32_t tok; };
static const _Fake _FAKES[] = {
  {"asleep",0,0,0,false,0}, {"one idle",1,0,0,false,12000},
  {"busy",4,3,0,false,89000}, {"attention",2,1,1,false,45000},
  {"completed",1,0,0,true,142000},
};

inline void dataSetDemo(bool on) {
  _demoMode = on;
  if (on) { _demoIdx = 0; _demoNext = millis(); }
}
inline bool dataDemo() { return _demoMode; }

inline bool dataConnected() {
  return _lastLiveMs != 0 && (millis() - _lastLiveMs) <= 30000;
}

inline bool dataBtActive() {
  // Desktop's idle keepalive is ~10s; give it 1.5x headroom.
  return _lastBtByteMs != 0 && (millis() - _lastBtByteMs) <= 15000;
}

inline const char* dataScenarioName() {
  if (_demoMode) return _FAKES[_demoIdx].n;
  if (dataConnected()) return dataBtActive() ? "ble" : "usb";
  return "none";
}

// Set true once the bridge sends a time sync — until then the RTC may
// hold whatever was on the coin cell (or 2000-01-01 if it lost power).
static bool _rtcValid = false;
inline bool dataRtcValid() { return _rtcValid; }
// Seed validity from a backup-kept hardware RTC at boot (see main setup), so
// the clock is trusted without waiting for a bridge re-sync. The bridge still
// overwrites with authoritative time when it connects.
inline void dataSetRtcValid(bool v) { _rtcValid = v; }

// Forward to the real xfer.h handler defined in xfer.h (Stage 4c).
// Declared as a free function so the linker pairs the call site here with
// the inline definition in the single .cpp that includes xfer.h.
bool xferCommand(JsonDocument& doc);

// Tell xfer.h which channel the next dispatched line came from, so its
// acks return on the same channel (USB → Serial, BLE → NUS).
void xferSetSource(bool fromUsb);

// Sanitise incoming text into the built-in 6x8 font's character set: strip
// control bytes, pass ASCII through, romanise Cyrillic via _cyrillicTranslit,
// map a few common punctuation marks, and fall back to '?' for anything else
// (emoji / CJK). The panel has no working Cyrillic glyph font, so romanised
// Latin is the readable option. The "ascii" name predates this and is kept to
// avoid churning every call site.
// Cyrillic → Latin transliteration. The bundled u8g2 "cyrillic" font turned
// out not to actually render Cyrillic glyphs on this panel, and there's no
// real Cyrillic bitmap font to embed, so we romanise instead — readable
// Latin beats blank/garbage. RU + UA coverage.
static const char* _cyrillicTranslit(uint32_t cp) {
  switch (cp) {
    // Russian uppercase A..Я
    case 0x0410: return "A";    case 0x0411: return "B";
    case 0x0412: return "V";    case 0x0413: return "H";   // Г → H (UA) / G (RU)
    case 0x0414: return "D";    case 0x0415: return "E";
    case 0x0416: return "Zh";   case 0x0417: return "Z";
    case 0x0418: return "I";    case 0x0419: return "Y";
    case 0x041A: return "K";    case 0x041B: return "L";
    case 0x041C: return "M";    case 0x041D: return "N";
    case 0x041E: return "O";    case 0x041F: return "P";
    case 0x0420: return "R";    case 0x0421: return "S";
    case 0x0422: return "T";    case 0x0423: return "U";
    case 0x0424: return "F";    case 0x0425: return "Kh";
    case 0x0426: return "Ts";   case 0x0427: return "Ch";
    case 0x0428: return "Sh";   case 0x0429: return "Shch";
    case 0x042A: return "'";    case 0x042B: return "Y";
    case 0x042C: return "'";    case 0x042D: return "E";
    case 0x042E: return "Yu";   case 0x042F: return "Ya";
    // Lowercase
    case 0x0430: return "a";    case 0x0431: return "b";
    case 0x0432: return "v";    case 0x0433: return "h";
    case 0x0434: return "d";    case 0x0435: return "e";
    case 0x0436: return "zh";   case 0x0437: return "z";
    case 0x0438: return "i";    case 0x0439: return "y";
    case 0x043A: return "k";    case 0x043B: return "l";
    case 0x043C: return "m";    case 0x043D: return "n";
    case 0x043E: return "o";    case 0x043F: return "p";
    case 0x0440: return "r";    case 0x0441: return "s";
    case 0x0442: return "t";    case 0x0443: return "u";
    case 0x0444: return "f";    case 0x0445: return "kh";
    case 0x0446: return "ts";   case 0x0447: return "ch";
    case 0x0448: return "sh";   case 0x0449: return "shch";
    case 0x044A: return "'";    case 0x044B: return "y";
    case 0x044C: return "'";    case 0x044D: return "e";
    case 0x044E: return "yu";   case 0x044F: return "ya";
    // Ukrainian-specific
    case 0x0404: return "Ye";   case 0x0454: return "ye";
    case 0x0406: return "I";    case 0x0456: return "i";
    case 0x0407: return "Yi";   case 0x0457: return "yi";
    case 0x0490: return "G";    case 0x0491: return "g";
    // Yo (used in RU)
    case 0x0401: return "Yo";   case 0x0451: return "yo";
    default:     return nullptr;
  }
}

static void _asciiCopy(char* dst, size_t dstLen, const char* src) {
  if (!dstLen) return;
  size_t j = 0, i = 0;
  while (src[i] && j + 1 < dstLen) {
    unsigned char c = (unsigned char)src[i];
    if (c < 0x20) { i++; continue; }                       // control byte
    if (c < 0x80) { dst[j++] = (char)c; i++; continue; }   // ASCII passthrough
    // Decode one UTF-8 multibyte sequence into a codepoint.
    uint32_t cp; int n;
    if      ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 3; }
    else { i++; continue; }                                // stray continuation
    i++;
    while (n-- > 0 && ((unsigned char)src[i] & 0xC0) == 0x80) {
      cp = (cp << 6) | ((unsigned char)src[i] & 0x3F); i++;
    }
    const char* t = _cyrillicTranslit(cp);                 // Cyrillic → Latin
    if (!t) switch (cp) {                                  // common punctuation
      case 0x2014: case 0x2013: t = "-";   break;          // em / en dash
      case 0x2018: case 0x2019: t = "'";   break;          // curly single quote
      case 0x201C: case 0x201D: t = "\"";  break;          // curly double quote
      case 0x2026:              t = "...";  break;          // ellipsis
      default: break;
    }
    if (t) { for (; *t && j + 1 < dstLen; t++) dst[j++] = *t; }
    else if (j + 1 < dstLen) dst[j++] = '?';               // unknown (emoji/CJK)
  }
  dst[j] = 0;
}

static void _applyJson(const char* line, TamaState* out) {
  JsonDocument doc;
  DeserializationError jerr = deserializeJson(doc, line);
  if (jerr) {
    // A line that doesn't parse is a dropped/garbled byte on the wire.
    // During an xfer the sender waits forever for an ack we'll never
    // send, so make the failure visible instead of silently eating it.
    Serial.printf("[data] json err: %s len=%u head=%.32s\n",
                  jerr.c_str(), (unsigned)strlen(line), line);
    return;
  }
  if (xferCommand(doc)) { _lastLiveMs = millis(); return; }

  // Bridge sends {"time":[epoch_sec, tz_offset_sec]}; gmtime_r on the
  // adjusted epoch yields local components including weekday.
  JsonArray t = doc["time"];
  if (!t.isNull() && t.size() == 2) {
    time_t local = (time_t)t[0].as<uint32_t>() + (int32_t)t[1];
    struct tm lt; gmtime_r(&local, &lt);
    RtcTime tm = { (uint8_t)lt.tm_hour, (uint8_t)lt.tm_min, (uint8_t)lt.tm_sec };
    RtcDate dt = { (uint16_t)(lt.tm_year + 1900), (uint8_t)(lt.tm_mon + 1),
                   (uint8_t)lt.tm_mday, (uint8_t)lt.tm_wday };
    rtcSetDateTime(dt, tm);
    _rtcValid = true;
    _lastLiveMs = millis();
    return;
  }

  out->sessionsTotal     = doc["total"]     | out->sessionsTotal;
  out->sessionsRunning   = doc["running"]   | out->sessionsRunning;
  out->sessionsWaiting   = doc["waiting"]   | out->sessionsWaiting;
  out->recentlyCompleted = doc["completed"] | false;
  uint32_t bridgeTokens = doc["tokens"] | 0;
  if (doc["tokens"].is<uint32_t>()) statsOnBridgeTokens(bridgeTokens);
  out->tokensToday = doc["tokens_today"] | out->tokensToday;
  const char* m = doc["msg"];
  if (m) _asciiCopy(out->msg, sizeof(out->msg), m);
  JsonArray la = doc["entries"];
  if (!la.isNull()) {
    uint8_t n = 0;
    for (JsonVariant v : la) {
      if (n >= TAMA_MAX_LINES) break;
      const char* s = v.as<const char*>();
      _asciiCopy(out->lines[n], sizeof(out->lines[n]), s ? s : "");
      n++;
    }
    if (n != out->nLines || (n > 0 && strcmp(out->lines[n-1], out->msg) != 0)) {
      out->lineGen++;
    }
    out->nLines = n;
  }
  JsonObject pr = doc["prompt"];
  if (!pr.isNull()) {
    const char* pid = pr["id"]; const char* pt = pr["tool"]; const char* ph = pr["hint"];
    _asciiCopy(out->promptId,   sizeof(out->promptId),   pid ? pid : "");
    _asciiCopy(out->promptTool, sizeof(out->promptTool), pt  ? pt  : "");
    _asciiCopy(out->promptHint, sizeof(out->promptHint), ph  ? ph  : "");
  } else {
    out->promptId[0] = 0; out->promptTool[0] = 0; out->promptHint[0] = 0;
  }
  out->lastUpdated = millis();
  _lastLiveMs = millis();
}

template<size_t N>
struct _LineBuf {
  char buf[N];
  uint16_t len = 0;
  void feed(Stream& s, TamaState* out) {
    while (s.available()) {
      char c = s.read();
      if (c == '\n' || c == '\r') {
        if (len > 0) { buf[len]=0; if (buf[0]=='{') _applyJson(buf, out); len=0; }
      } else if (len < N-1) {
        buf[len++] = c;
      }
    }
  }
};

static _LineBuf<1024> _usbLine, _btLine;

inline void dataPoll(TamaState* out) {
  uint32_t now = millis();

  if (_demoMode) {
    if (now >= _demoNext) { _demoIdx = (_demoIdx + 1) % 5; _demoNext = now + 8000; }
    const _Fake& s = _FAKES[_demoIdx];
    out->sessionsTotal=s.t; out->sessionsRunning=s.r; out->sessionsWaiting=s.w;
    out->recentlyCompleted=s.c; out->tokensToday=s.tok; out->lastUpdated=now;
    out->connected = true;
    snprintf(out->msg, sizeof(out->msg), "demo: %s", s.n);
    return;
  }

  xferSetSource(true);                 // lines below come over USB-CDC
  _usbLine.feed(Serial, out);
  xferSetSource(false);                // and these over BLE
  // BLE ring buffer is drained manually since it's not a Stream.
  while (bleAvailable()) {
    int c = bleRead();
    if (c < 0) break;
    _lastBtByteMs = millis();
    if (c == '\n' || c == '\r') {
      if (_btLine.len > 0) {
        _btLine.buf[_btLine.len] = 0;
        if (_btLine.buf[0] == '{') _applyJson(_btLine.buf, out);
        _btLine.len = 0;
      }
    } else if (_btLine.len < sizeof(_btLine.buf) - 1) {
      _btLine.buf[_btLine.len++] = (char)c;
    }
  }

  out->connected = dataConnected();
  if (!out->connected) {
    out->sessionsTotal=0; out->sessionsRunning=0; out->sessionsWaiting=0;
    out->recentlyCompleted=false; out->lastUpdated=now;
    strncpy(out->msg, "No Claude connected", sizeof(out->msg)-1);
    out->msg[sizeof(out->msg)-1]=0;
  }
}
