#include "gesture.h"
#include "hal/touch.h"
#include "logic/classify.h"
#include <Arduino.h>
#include <stdlib.h>

// Gesture state machine — see gesture.h.
//
// Note on naming: globals avoid x0/y0/xl/yl because <math.h> exposes the
// Bessel functions y0() / y1() at file scope and the linker takes file-
// scope identifiers as symbols (the conflict shows up as nonsense errors
// about "assignment to function double y0(double)").

static bool         penDown = false;
static uint16_t     gStartX = 0, gStartY = 0;   // touch-down coords
static uint16_t     gLastX  = 0, gLastY  = 0;   // last seen during drag
static uint32_t     gStartT = 0;                // touch-down millis
static GestureEvent pending = { GESTURE_NONE, 0, 0, 0, 0, 0 };

void gestureUpdate() {
  uint16_t x, y;
  bool down = touchRead(&x, &y);

  if (down && !penDown) {
    // Edge: rising. Start a new gesture.
    penDown = true;
    gStartX = gLastX = x;
    gStartY = gLastY = y;
    gStartT = millis();
    return;
  }

  if (down) {
    // Mid-drag: track the latest position so dx/dy uses the actual end.
    gLastX = x;
    gLastY = y;
    return;
  }

  if (!down && penDown) {
    // Edge: falling. Classify the gesture and post it.
    penDown = false;
    int16_t  dx   = (int16_t)gLastX - (int16_t)gStartX;
    int16_t  dy   = (int16_t)gLastY - (int16_t)gStartY;
    uint16_t dtMs = (uint16_t)min<uint32_t>(millis() - gStartT, 65535u);

    // Classification lives in logic/classify.h so the host test suite
    // covers the approve-vs-deny decision edges.
    GestureEvent ev = { classifyGesture(dx, dy, dtMs), gStartX, gStartY, dx, dy, dtMs };

    // Don't overwrite an unread pending event with NONE; otherwise the new
    // event always wins (a real gesture overrides a stale GESTURE_NONE).
    if (ev.kind != GESTURE_NONE) pending = ev;
  }
}

GestureEvent gestureGet() {
  GestureEvent out = pending;
  pending.kind = GESTURE_NONE;
  return out;
}
