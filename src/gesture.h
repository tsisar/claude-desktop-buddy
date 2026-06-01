#pragma once
#include <stdint.h>

// Gesture recognition layer on top of touch HAL (hal/touch.h).
//
// State machine: pen-down → start (record x0,y0,t0); finger movement is
// ignored; pen-up → classify by total displacement and elapsed time:
//
//   |dx| < TAP_R && |dy| < TAP_R && dt < TAP_MS   → TAP at (x0,y0)
//   |dx|  > SWIPE_THR && |dx| > |dy|              → SWIPE_LEFT / RIGHT
//   |dy|  > SWIPE_THR && |dy| > |dx|              → SWIPE_UP   / DOWN
//   otherwise                                     → NONE  (ignored)
//
// Events are one-shot: gestureGet() returns the pending event and clears
// it. Call gestureUpdate() every loop tick; gestureGet() once per tick to
// consume. The decision happens on release (no live-fire during drag), so
// approve/deny swipes register the moment the finger lifts.

enum GestureKind : uint8_t {
  GESTURE_NONE = 0,
  GESTURE_TAP,
  GESTURE_SWIPE_UP,
  GESTURE_SWIPE_DOWN,
  GESTURE_SWIPE_LEFT,
  GESTURE_SWIPE_RIGHT,
};

struct GestureEvent {
  GestureKind kind;
  uint16_t    x;     // for TAP: touch coords; for SWIPE: start coords
  uint16_t    y;
  int16_t     dx;    // total displacement, signed; 0 for TAP
  int16_t     dy;
  uint16_t    dtMs;  // elapsed time pen-down → pen-up
};

// Pump the recognizer. Call once per main-loop tick — internally polls
// touchRead() and updates state. Cheap when nothing is happening.
void gestureUpdate();

// Read the latest event and consume it. Returns GESTURE_NONE if nothing
// new since the last call.
GestureEvent gestureGet();

// Tuning (compile-time defaults; expose if a stage harness wants to tweak).
//   TAP_R     — max movement (px) to still count as a tap
//   TAP_MS    — max contact time (ms) for a tap
//   SWIPE_THR — min displacement (px) on the dominant axis for a swipe
// On a 368×448 panel these defaults give finger-comfortable thresholds.
static constexpr int GESTURE_TAP_R   = 15;
static constexpr int GESTURE_TAP_MS  = 300;
static constexpr int GESTURE_SWIPE_T = 80;
