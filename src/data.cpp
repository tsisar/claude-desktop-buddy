#include "data.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <string.h>
#include <time.h>
#include "ble_bridge.h"
#include "hal/rtc.h"
#include "stats.h"
#include "xfer.h"
#include "logic/translit.h"

// AMOLED port of the data pump. Same public API + TamaState layout as the
// M5 build — so any UI code from the M5 main.cpp ports verbatim — but the
// RTC sync path goes through hal/rtc.h instead of M5.Rtc.

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

void dataSetDemo(bool on) {
  _demoMode = on;
  if (on) { _demoIdx = 0; _demoNext = millis(); }
}
bool dataDemo() { return _demoMode; }

bool dataConnected() {
  return _lastLiveMs != 0 && (millis() - _lastLiveMs) <= 30000;
}

bool dataBtActive() {
  // Desktop's idle keepalive is ~10s; give it 1.5x headroom.
  return _lastBtByteMs != 0 && (millis() - _lastBtByteMs) <= 15000;
}

const char* dataScenarioName() {
  if (_demoMode) return _FAKES[_demoIdx].n;
  if (dataConnected()) return dataBtActive() ? "ble" : "usb";
  return "none";
}

// Set true once the bridge sends a time sync — until then the RTC may
// hold whatever was on the coin cell (or 2000-01-01 if it lost power).
static bool _rtcValid = false;
bool dataRtcValid() { return _rtcValid; }
void dataSetRtcValid(bool v) { _rtcValid = v; }

// Text sanitisation (UTF-8 -> 6x8 font charset, Cyrillic romanisation)
// lives in logic/translit.h so the host test suite covers the decoder.

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
    uint32_t epoch = t[0].as<uint32_t>();
    int32_t  tz    = t[1];
    // Sanity-gate before touching the RTC: a buggy bridge sending
    // {"time":[0,0]} or a garbage tz offset must not set the clock to 1970
    // and latch _rtcValid. Mirrors the >=2024 guard on the boot-restore
    // path in main.cpp. A rejected sync still proves the bridge is alive.
    if (epoch < 1704067200UL /* 2024-01-01 UTC */ ||
        tz < -14 * 3600 || tz > 14 * 3600) {
      Serial.printf("[data] time sync rejected: epoch=%lu tz=%ld\n",
                    (unsigned long)epoch, (long)tz);
      _lastLiveMs = millis();
      return;
    }
    time_t local = (time_t)epoch + tz;
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
  if (m) asciiCopy(out->msg, sizeof(out->msg), m);
  JsonArray la = doc["entries"];
  if (!la.isNull()) {
    uint8_t n = 0;
    for (JsonVariant v : la) {
      if (n >= TAMA_MAX_LINES) break;
      const char* s = v.as<const char*>();
      asciiCopy(out->lines[n], sizeof(out->lines[n]), s ? s : "");
      n++;
    }
    out->nLines = n;
  }
  JsonObject pr = doc["prompt"];
  if (!pr.isNull()) {
    const char* pid = pr["id"]; const char* pt = pr["tool"]; const char* ph = pr["hint"];
    asciiCopy(out->promptId,   sizeof(out->promptId),   pid ? pid : "");
    asciiCopy(out->promptTool, sizeof(out->promptTool), pt  ? pt  : "");
    asciiCopy(out->promptHint, sizeof(out->promptHint), ph  ? ph  : "");
  } else {
    out->promptId[0] = 0; out->promptTool[0] = 0; out->promptHint[0] = 0;
  }
  out->lastUpdated = millis();
  _lastLiveMs = millis();
}

// A line longer than the buffer used to be silently truncated at N-1 and
// parsed as broken JSON — a heartbeat carrying a full 16-entry transcript
// can exceed 1 KB, so a verbose desktop session rendered every heartbeat
// undecodable. Now: 4 KB buffers (matches the 4 KB Serial RX ring set in
// setup() and the desktop-side 4 KB turn-event cap), and an explicit
// overflow flag so an over-long line is consumed to its newline and
// dropped ONCE with a log line, instead of fusing into parse noise.
template<size_t N>
struct _LineBuf {
  char buf[N];
  uint16_t len = 0;
  bool overflow = false;
  void feed(Stream& s, TamaState* out) {
    while (s.available()) {
      char c = s.read();
      if (c == '\n' || c == '\r') {
        if (overflow) {
          Serial.printf("[data] line overflow >%uB, dropped\n", (unsigned)(N - 1));
          overflow = false; len = 0;
        } else if (len > 0) {
          buf[len]=0; if (buf[0]=='{') _applyJson(buf, out); len=0;
        }
      } else if (len < N-1) {
        buf[len++] = c;
      } else {
        overflow = true;   // keep consuming to the newline, then drop
      }
    }
  }
};

static _LineBuf<4096> _usbLine, _btLine;

void dataPoll(TamaState* out) {
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
  static bool _btDiscard = false;      // resync: drop bytes until next newline
  while (bleAvailable()) {
    if (bleRxOverflowTake()) {
      // The BLE RX ring dropped bytes while the loop was blocked: whatever
      // is buffered is a truncated fragment that would fuse with the next
      // write into one garbled line (and a lost ack hangs the ack-paced
      // host). Reset and resync on a clean line boundary.
      Serial.printf("[data] ble rx overflow, %lu B dropped total\n",
                    (unsigned long)bleRxDropped());
      _btLine.len = 0;
      _btLine.overflow = false;
      _btDiscard = true;
    }
    int c = bleRead();
    if (c < 0) break;
    _lastBtByteMs = millis();
    if (c == '\n' || c == '\r') {
      if (_btDiscard) {
        _btDiscard = false; _btLine.len = 0;
      } else if (_btLine.overflow) {
        Serial.println("[data] ble line overflow, dropped");
        _btLine.overflow = false; _btLine.len = 0;
      } else if (_btLine.len > 0) {
        _btLine.buf[_btLine.len] = 0;
        if (_btLine.buf[0] == '{') _applyJson(_btLine.buf, out);
        _btLine.len = 0;
      }
    } else if (_btLine.len < sizeof(_btLine.buf) - 1) {
      _btLine.buf[_btLine.len++] = (char)c;
    } else {
      _btLine.overflow = true;
    }
  }

  out->connected = dataConnected();
  if (!out->connected) {
    xferAbort();   // no-op unless a transfer was open when the link died
    out->sessionsTotal=0; out->sessionsRunning=0; out->sessionsWaiting=0;
    out->recentlyCompleted=false; out->lastUpdated=now;
    // Drop any pending prompt with the link: the host that asked is gone.
    // Leaving promptId set pinned the approval screen forever, blocked auto
    // screen-off and nap detection, and let a swipe answer a prompt the
    // host had already abandoned.
    out->promptId[0]=0; out->promptTool[0]=0; out->promptHint[0]=0;
    strncpy(out->msg, "No Claude connected", sizeof(out->msg)-1);
    out->msg[sizeof(out->msg)-1]=0;
  }
}
