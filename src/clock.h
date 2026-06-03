#pragma once

// Clock UI — the top-left HH:MM widget and the full-screen clock face.
// Split out of main.cpp's render layer. Backed by the RTC HAL (hal/rtc.h)
// and the shared Surface (gfx), reached the same way main.cpp's other render
// helpers reach them. The face replaces the home view when the device is
// parked on USB, RTC-synced and idle; the small widget rides along the
// home / PET views.

// Small HH:MM at (x, y) — middle-left anchor, mirroring the battery widget
// on the right. Purely renders the current RTC time (no-op if the clock
// can't be read). The caller gates visibility — RTC-synced + the "battery"
// UI setting — because that state is main.cpp-only (see clock.cpp).
void clockDrawWidget(int x, int y);

// Full-screen clock face: big HH:MM, seconds, weekday/month/date line, and
// the battery widget top-right. Clears the sprite itself.
void clockDrawFace();