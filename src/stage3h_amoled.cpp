// Stage 3h — touch + gesture verification harness.
//
// Full-screen canvas. Last recognised gesture is drawn:
//   TAP    → coloured dot at the touch point
//   SWIPE  → arrow from start point in the direction, with label
// A header line at top shows the gesture name + dt/dx/dy. Older taps fade
// to grey so repeated taps leave a trail.
//
// Build switch: enable just this file in build_src_filter for ws-amoled-18,
// disable stage3g (they both define setup()/loop()).

#include <Arduino.h>
#include <Wire.h>
#include "board_pins.h"
#include "hal/display.h"
#include "hal/power.h"
#include "hal/touch.h"
#include "gesture.h"

static Surface gfx;
static bool    dispOk = false;
static const int W = LCD_WIDTH, H = LCD_HEIGHT;

// Trail of recent tap dots so you can see what registered without juggling
// a serial monitor. Newest at the head.
struct Dot { uint16_t x, y; uint32_t at; };
static Dot     dots[8];
static uint8_t dotHead = 0;
static void pushDot(uint16_t x, uint16_t y) {
  dots[dotHead] = { x, y, millis() };
  dotHead = (dotHead + 1) % 8;
}

static GestureEvent lastEv = { GESTURE_NONE, 0, 0, 0, 0, 0 };

static const char* gestureName(GestureKind k) {
  switch (k) {
    case GESTURE_TAP:         return "TAP";
    case GESTURE_SWIPE_UP:    return "SWIPE UP";
    case GESTURE_SWIPE_DOWN:  return "SWIPE DOWN";
    case GESTURE_SWIPE_LEFT:  return "SWIPE LEFT";
    case GESTURE_SWIPE_RIGHT: return "SWIPE RIGHT";
    default:                  return "—";
  }
}

static void drawArrow(int x0, int y0, int dx, int dy, uint16_t col) {
  // Clamp arrow length to keep it on-screen for huge swipes.
  int len = (int)sqrtf((float)dx * dx + (float)dy * dy);
  if (len < 1) return;
  int draw = min(len, 200);
  int x1 = x0 + dx * draw / len;
  int y1 = y0 + dy * draw / len;
  // Thick shaft: three parallel lines.
  for (int o = -1; o <= 1; o++) {
    gfx.drawLine(x0 + o, y0, x1 + o, y1, col);
    gfx.drawLine(x0, y0 + o, x1, y1 + o, col);
  }
  // Arrow head: triangle perpendicular to direction.
  float nx = (float)dx / len, ny = (float)dy / len;
  float px = -ny, py = nx;   // perpendicular
  int hx = x1, hy = y1;
  int bx = (int)(x1 - nx * 18), by = (int)(y1 - ny * 18);
  int lx = (int)(bx + px * 10), ly = (int)(by + py * 10);
  int rx = (int)(bx - px * 10), ry = (int)(by - py * 10);
  gfx.fillTriangle(hx, hy, lx, ly, rx, ry, col);
}

void setup() {
  Serial.begin(115200); Serial.setTxTimeoutMs(0); delay(300);
  Serial.println("\n[stage3h] touch + gesture verification");

  Wire.begin(IIC_SDA, IIC_SCL, 400000);
  powerInit(Wire);
  dispOk = gfx.begin();
  touchInit(Wire);

  Serial.printf("[stage3h] disp=%d touch=%d pmu=%d\n",
                dispOk, touchOk(), powerOk());
}

void loop() {
  static uint32_t nextDraw = 0;
  uint32_t now = millis();

  gestureUpdate();
  GestureEvent ev = gestureGet();
  if (ev.kind != GESTURE_NONE) {
    lastEv = ev;
    if (ev.kind == GESTURE_TAP) pushDot(ev.x, ev.y);
    Serial.printf("[stage3h] %-12s at (%u,%u)  dx=%d dy=%d  dt=%ums\n",
                  gestureName(ev.kind), ev.x, ev.y, ev.dx, ev.dy, ev.dtMs);
  }

  if (!dispOk || now < nextDraw) { delay(8); return; }
  nextDraw = now + 50;   // 20 fps is plenty for the visualisation

  gfx.fillSprite(0x0000);

  // Header — last gesture + numbers.
  gfx.setTextDatum(TL_DATUM);
  gfx.setTextSize(3);
  gfx.setTextColor(0xFFFF, 0x0000);
  gfx.setCursor(10, 10); gfx.print("GESTURE");

  gfx.setTextSize(4);
  gfx.setTextColor(0xFFE0, 0x0000);
  gfx.setCursor(10, 50); gfx.print(gestureName(lastEv.kind));

  if (lastEv.kind != GESTURE_NONE) {
    gfx.setTextSize(2);
    gfx.setTextColor(0xC618, 0x0000);
    gfx.setCursor(10, 100);
    gfx.printf("dx=%d  dy=%d  dt=%ums", lastEv.dx, lastEv.dy, lastEv.dtMs);
    gfx.setCursor(10, 122);
    gfx.printf("start (%u, %u)", lastEv.x, lastEv.y);
  }

  gfx.drawFastHLine(0, 150, W, 0x4208);

  // Tap dots — fade by age (newer = brighter / cyan; older = dim grey).
  for (int i = 0; i < 8; i++) {
    if (dots[i].at == 0) continue;
    uint32_t age = now - dots[i].at;
    if (age > 5000) continue;
    uint8_t bright = (uint8_t)(255 - (age * 255 / 5000));
    uint16_t col;
    if (bright > 200) col = 0x07FF;        // cyan, newest
    else if (bright > 100) col = 0x8410;   // mid-grey
    else col = 0x4208;                     // dim
    gfx.fillCircle(dots[i].x, dots[i].y, 8, col);
    gfx.drawCircle(dots[i].x, dots[i].y, 12, col);
  }

  // Arrow for the last swipe (drawn on top so it's always visible).
  if (lastEv.kind == GESTURE_SWIPE_UP || lastEv.kind == GESTURE_SWIPE_DOWN ||
      lastEv.kind == GESTURE_SWIPE_LEFT || lastEv.kind == GESTURE_SWIPE_RIGHT) {
    drawArrow(lastEv.x, lastEv.y, lastEv.dx, lastEv.dy, 0xFFE0);
  }

  // Footer hint
  gfx.setTextSize(2);
  gfx.setTextColor(0x4208, 0x0000);
  gfx.setCursor(10, H - 30);
  gfx.print("tap / swipe anywhere");

  gfx.pushSprite();
  delay(8);
}
