#pragma once
#include <stdint.h>
#include <ArduinoJson.h>

// Character-pack transfer + housekeeping command handler. Same wire
// protocol as the M5 build (REFERENCE.md): cmd:char_begin / cmd:file /
// cmd:chunk / cmd:file_end / cmd:char_end, plus cmd:status / name /
// owner / species / unpair and the debug-only cmd:pose / cmd:shot.
//
// Declarations only — state and bodies live in xfer.cpp (this used to be
// header-only with file-static state and the same one-TU-only hazard the
// stats module had).

// Dispatch one parsed JSON command line. Returns true if the command was
// claimed (handled or deliberately swallowed), false if it isn't a command
// and should fall through to the heartbeat/state parser.
bool xferCommand(JsonDocument& doc);

// Which channel the line being dispatched arrived on — set by dataPoll()
// before it feeds USB or BLE bytes into the parser. Replies go back on
// the same channel (USB → Serial, BLE → NUS).
void xferSetSource(bool fromUsb);

bool     xferActive();
uint32_t xferProgress();
uint32_t xferTotal();

// Abort an interrupted transfer — called by dataPoll() when the liveness
// window expires mid-push so a dead host can't pin the progress screen
// (and leak the open file handle) forever.
void xferAbort();

// Wipe /characters/ — backs the "delete char" reset path.
void xferDeleteAll();
