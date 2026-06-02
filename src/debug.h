#pragma once
#include <Arduino.h>

// Verbose debug logging gate.
//
// The firmware mirrors a fair amount onto the serial console: the BLE wire
// protocol (every status heartbeat, every ack), GIF rotations, and per-touch
// / per-button events. That's handy at a desk with a serial monitor attached
// but it's a steady stream of work the device does for nobody once it's
// running standalone — and the heartbeat mirror in particular prints the full
// status JSON on every host poll.
//
// VLOG()/VLOGLN() compile to nothing unless BUDDY_VERBOSE_LOG is set to 1
// (e.g. add -DBUDDY_VERBOSE_LOG=1 to build_flags). One-shot boot/init logs and
// genuine error paths keep using Serial.* directly so they always show.
#ifndef BUDDY_VERBOSE_LOG
#define BUDDY_VERBOSE_LOG 0
#endif

#if BUDDY_VERBOSE_LOG
#define VLOG(...)       Serial.printf(__VA_ARGS__)
#define VLOGLN(s)       Serial.println(s)
#define VWRITE(b, len)  Serial.write((b), (len))
#else
#define VLOG(...)       ((void)0)
#define VLOGLN(s)       ((void)0)
#define VWRITE(b, len)  ((void)0)
#endif
