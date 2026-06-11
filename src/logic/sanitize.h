#pragma once
#include <stddef.h>
#include <string.h>

// Pure, Arduino-free logic — host-testable via `pio test -e native`.
//
// Accept only a plain single-path-segment name from an untrusted peer:
// [A-Za-z0-9._-], non-empty, not "." / "..", shorter than maxLen. Blocks
// path traversal ("../etc"), absolute paths and separators — without this,
// names from the wire flow straight into snprintf'd filesystem paths.
// Char packs are flat (no subdirs), so one segment is enough.
inline bool buddySafeName(const char* s, size_t maxLen) {
  if (!s || !*s) return false;
  size_t n = 0;
  for (const char* c = s; *c; c++, n++) {
    char ch = *c;
    bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-';
    if (!ok) return false;
  }
  if (n >= maxLen) return false;
  if (strcmp(s, ".") == 0 || strcmp(s, "..") == 0) return false;
  return true;
}
