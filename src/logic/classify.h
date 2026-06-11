#pragma once
#include <stdint.h>
#include "../gesture.h"

// Pure, Arduino-free logic — host-testable via `pio test -e native`.
//
// Pen-up classifier: total displacement + contact time → gesture kind.
// This is the function that decides approve vs deny, so its edges are
// covered by host tests (test/test_logic).
inline GestureKind classifyGesture(int16_t dx, int16_t dy, uint16_t dtMs) {
  int adx = dx < 0 ? -dx : dx;
  int ady = dy < 0 ? -dy : dy;
  if (adx < GESTURE_TAP_R && ady < GESTURE_TAP_R && dtMs < GESTURE_TAP_MS)
    return GESTURE_TAP;
  if (adx >= GESTURE_SWIPE_T && adx > ady)
    return (dx > 0) ? GESTURE_SWIPE_RIGHT : GESTURE_SWIPE_LEFT;
  if (ady >= GESTURE_SWIPE_T && ady > adx)
    return (dy > 0) ? GESTURE_SWIPE_DOWN : GESTURE_SWIPE_UP;
  return GESTURE_NONE;   // ambiguous slow move — drop it
}
