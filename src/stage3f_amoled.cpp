// Stage 3f — the buddy, alive: species state driven by the real Claude
// heartbeat over BLE, and APPROVE/DENY that actually send a permission
// decision back. Combines stage3c (18 species via Surface) + the NimBLE
// bridge + a trimmed heartbeat parser (avoids data.h's M5.Axp/M5.Rtc deps;
// the real data.h gets ported when this folds into main.cpp).
//
// Wire protocol (REFERENCE.md): newline-delimited JSON.
//   heartbeat: {total,running,waiting,completed?,msg,entries[],tokens,
//               tokens_today, prompt?{id,tool,hint}}
//   we reply:  {"cmd":"permission","id":"<id>","decision":"once"|"deny"}
//
// build_src_filter: +<hal/> +<buddies/> +<ble_bridge_nimble.cpp> +<stage3f>

#include <Arduino.h>
#include <Wire.h>
#include <string.h>
#include <esp_mac.h>   // esp_read_mac / ESP_MAC_BT (core 3.x)
#include <ArduinoJson.h>
#include "board_pins.h"
#include "hal/display.h"
#include "ble_bridge.h"
#include "buddy.h"
#include "buddy_common.h"

static Surface gfx;
static bool dispOk = false, touchOk = false;
static const int W = LCD_WIDTH, H = LCD_HEIGHT;

// ── buddy_common.h symbol definitions (Surface backing; same as stage3c) ──
const int BUDDY_X_CENTER = W / 2;
const int BUDDY_CANVAS_W = W;
const int BUDDY_Y_BASE   = 30;
const int BUDDY_Y_OVERLAY= 6;
const int BUDDY_CHAR_W   = 6;
const int BUDDY_CHAR_H   = 8;
const uint16_t BUDDY_BG=0x0000, BUDDY_HEART=0xF810, BUDDY_DIM=0x8410,
  BUDDY_YEL=0xFFE0, BUDDY_WHITE=0xFFFF, BUDDY_CYAN=0x07FF, BUDDY_GREEN=0x07E0,
  BUDDY_PURPLE=0xA01F, BUDDY_RED=0xF800, BUDDY_BLUE=0x041F;
static const uint8_t SCALE = 4;

void buddyPrintLine(const char* line, int yPx, uint16_t color, int xOff) {
  int w = strlen(line) * BUDDY_CHAR_W * SCALE;
  gfx.setTextColor(color, BUDDY_BG);
  gfx.setCursor(BUDDY_X_CENTER - w/2 + xOff*SCALE, yPx);
  gfx.print(line);
}
void buddyPrintSprite(const char* const* lines, uint8_t n, int yOffset, uint16_t color, int xOff) {
  gfx.setTextSize(SCALE);
  int yBase = BUDDY_Y_BASE * SCALE - (SCALE - 1) * 14;
  for (uint8_t i = 0; i < n; i++)
    buddyPrintLine(lines[i], yBase + (yOffset + i*BUDDY_CHAR_H)*SCALE, color, xOff);
}
void buddySetCursor(int x, int y) { gfx.setCursor(BUDDY_X_CENTER + (x-BUDDY_X_CENTER)*SCALE, y*SCALE); }
void buddySetColor(uint16_t fg) { gfx.setTextColor(fg, BUDDY_BG); }
void buddyPrint(const char* s) { gfx.setTextSize(SCALE); gfx.print(s); }

// pick the cat (matches the buddy the bringup used)
extern const Species CAT_SPECIES;
static const Species* SP = &CAT_SPECIES;

// PersonaState order matches src/main.cpp
enum { P_SLEEP, P_IDLE, P_BUSY, P_ATTENTION, P_CELEBRATE, P_DIZZY, P_HEART };
static const char* STATE_NAMES[7] = { "sleep","idle","busy","attention","celebrate","dizzy","heart" };

// ── live state from BLE heartbeat ──
struct Tama {
  uint8_t total=0, running=0, waiting=0;
  bool    completed=false;
  bool    connected=false;
  uint32_t tokensToday=0;
  char    msg[24]="";
  char    promptId[40]="";
  char    promptTool[20]="";
  char    promptHint[44]="";
  uint32_t lastMs=0;
} tama;

static uint32_t celebrateUntil = 0;

static uint8_t derive() {
  uint32_t age = millis() - tama.lastMs;
  if (!tama.connected || age > 30000) return P_SLEEP;   // no bridge → sleep
  if (tama.promptId[0])               return P_ATTENTION;
  if (tama.waiting > 0)               return P_ATTENTION;
  if (millis() < celebrateUntil)      return P_CELEBRATE;
  if (tama.running >= 3)              return P_BUSY;
  if (tama.running >= 1)              return P_BUSY;
  return P_IDLE;
}

// ── JSON line parse (trimmed; ignores time/owner/xfer for now) ──
static void applyJson(const char* line) {
  JsonDocument doc;
  if (deserializeJson(doc, line)) return;
  if (doc["cmd"].is<const char*>()) return;   // desktop's {"cmd":"status"} poll — ignore

  tama.total   = doc["total"]   | tama.total;
  tama.running = doc["running"] | tama.running;
  tama.waiting = doc["waiting"] | tama.waiting;
  bool wasCompleted = tama.completed;
  tama.completed = doc["completed"] | false;
  if (tama.completed && !wasCompleted) celebrateUntil = millis() + 3000;
  tama.tokensToday = doc["tokens_today"] | tama.tokensToday;
  const char* m = doc["msg"];
  if (m) { strncpy(tama.msg, m, sizeof(tama.msg)-1); tama.msg[sizeof(tama.msg)-1]=0; }

  JsonObject pr = doc["prompt"];
  if (!pr.isNull()) {
    const char* id=pr["id"], *tl=pr["tool"], *hn=pr["hint"];
    strncpy(tama.promptId,   id?id:"", sizeof(tama.promptId)-1);   tama.promptId[sizeof(tama.promptId)-1]=0;
    strncpy(tama.promptTool, tl?tl:"", sizeof(tama.promptTool)-1); tama.promptTool[sizeof(tama.promptTool)-1]=0;
    strncpy(tama.promptHint, hn?hn:"", sizeof(tama.promptHint)-1); tama.promptHint[sizeof(tama.promptHint)-1]=0;
  } else {
    tama.promptId[0]=tama.promptTool[0]=tama.promptHint[0]=0;
  }
  tama.connected = true;
  tama.lastMs = millis();
}

// line-buffer the BLE RX stream, parse on newline
static char lineBuf[1024]; static uint16_t lineLen = 0;
static void pollBle() {
  while (bleAvailable()) {
    int c = bleRead(); if (c < 0) break;
    if (c == '\n' || c == '\r') {
      if (lineLen) { lineBuf[lineLen]=0; if (lineBuf[0]=='{') applyJson(lineBuf); lineLen=0; }
    } else if (lineLen < sizeof(lineBuf)-1) lineBuf[lineLen++] = (char)c;
  }
}

static void sendPermission(const char* decision) {
  if (!tama.promptId[0]) return;
  char cmd[96];
  int n = snprintf(cmd, sizeof(cmd),
    "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"%s\"}\n", tama.promptId, decision);
  bleWrite((const uint8_t*)cmd, n);
  Serial.printf("[stage3f] sent %s for %s\n", decision, tama.promptId);
  tama.promptId[0]=0;   // consumed; clear so buttons deactivate until next prompt
}

// ── buttons ──
static const int BTN_H=96, BTN_TOP=H-BTN_H, BTN_GAP=8;
static const int BTN_W=(W-3*BTN_GAP)/2, APPROVE_X=BTN_GAP, DENY_X=BTN_GAP*2+BTN_W;
static void drawButtons(bool active, int pressed) {
  uint16_t g = active ? (pressed==1?0x07E0:0x02E0) : 0x10A2;  // grey when inactive
  uint16_t r = active ? (pressed==2?0xF800:0x7800) : 0x2104;
  gfx.fillRoundRect(APPROVE_X, BTN_TOP, BTN_W, BTN_H, 12, g);
  gfx.drawRoundRect(APPROVE_X, BTN_TOP, BTN_W, BTN_H, 12, active?0x07E0:0x4208);
  gfx.fillRoundRect(DENY_X, BTN_TOP, BTN_W, BTN_H, 12, r);
  gfx.drawRoundRect(DENY_X, BTN_TOP, BTN_W, BTN_H, 12, active?0xF800:0x4208);
  gfx.setTextDatum(MC_DATUM); gfx.setTextSize(3);
  gfx.setTextColor(active?0xFFFF:0x8410, g);
  gfx.drawString("APPROVE", APPROVE_X+BTN_W/2, BTN_TOP+BTN_H/2);
  gfx.setTextColor(active?0xFFFF:0x8410, r);
  gfx.drawString("DENY", DENY_X+BTN_W/2, BTN_TOP+BTN_H/2);
  gfx.setTextDatum(TL_DATUM);
}

static bool touchPt(uint16_t& x, uint16_t& y) {
  Wire.beginTransmission(TOUCH_ADDR); Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)TOUCH_ADDR, 5) != 5) return false;
  uint8_t n=Wire.read(), xh=Wire.read(), xl=Wire.read(), yh=Wire.read(), yl=Wire.read();
  if ((n & 0x0F) == 0) return false;
  x=((uint16_t)(xh&0x0F)<<8)|xl; y=((uint16_t)(yh&0x0F)<<8)|yl; return true;
}

void setup() {
  Serial.begin(115200); Serial.setTxTimeoutMs(0); delay(300);
  Serial.println("\n[stage3f] buddy alive over BLE");
  dispOk = gfx.begin();
  Wire.begin(IIC_SDA, IIC_SCL, 400000);
  Wire.beginTransmission(TOUCH_ADDR); touchOk = (Wire.endTransmission()==0);
  pinMode(TP_INT, INPUT);

  uint8_t mac[6]={0}; esp_read_mac(mac, ESP_MAC_BT);
  char name[16]; snprintf(name, sizeof(name), "Claude-%02X%02X", mac[4], mac[5]);
  bleInit(name);
  Serial.printf("[stage3f] disp=%d touch=%d ble=%s\n", dispOk, touchOk, name);
}

void loop() {
  static uint32_t nextTick=0; static uint32_t tick=0;
  static int flashBtn=0; static uint32_t flashUntil=0; static bool latch=false;
  uint32_t now = millis();

  pollBle();

  bool prompt = tama.promptId[0] != 0;
  uint16_t tx, ty;
  bool down = touchOk && touchPt(tx, ty);
  if (down && !latch) {
    latch = true;
    if (ty >= BTN_TOP && prompt) {
      if (tx < APPROVE_X + BTN_W) { flashBtn=1; sendPermission("once"); }
      else                        { flashBtn=2; sendPermission("deny"); }
      flashUntil = now + 300;
    }
  } else if (!down) latch = false;
  if (now >= flashUntil) flashBtn = 0;

  if (dispOk && now >= nextTick) {
    nextTick = now + 200; tick++;
    uint8_t st = derive();
    gfx.fillSprite(BUDDY_BG);
    StateFn fn = SP->states[st];
    if (fn) fn(tick);

    // header: BLE link + derived state (or the prompt tool when waiting)
    gfx.setTextDatum(TC_DATUM); gfx.setTextSize(2);
    char hdr[48];
    if (prompt) { gfx.setTextColor(0xFFE0, BUDDY_BG); snprintf(hdr,sizeof(hdr),"approve? %s", tama.promptTool); }
    else {
      bool linked = bleConnected();
      gfx.setTextColor(linked?0x07E0:BUDDY_DIM, BUDDY_BG);
      snprintf(hdr,sizeof(hdr), "%s  %s", linked?"BLE":"...", STATE_NAMES[st]);
    }
    gfx.drawString(hdr, W/2, BTN_TOP - 30);
    gfx.setTextDatum(TL_DATUM);

    drawButtons(prompt, flashBtn);
    gfx.pushSprite();
  }
  delay(8);
}
