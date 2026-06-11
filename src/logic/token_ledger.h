#pragma once
#include <stdint.h>

// Pure, Arduino-free logic — host-testable via `pio test -e native`.
//
// The bridge reports its cumulative token total since IT started; the
// device credits deltas. Two traps this struct encodes:
//   • device reboot: our last-seen resets to 0 while the bridge total
//     doesn't — crediting the first packet would re-credit the entire
//     session, so the first sight only latches;
//   • bridge restart: the total DROPS — resync without crediting.
struct TokenLedger {
  uint32_t last   = 0;
  bool     synced = false;

  // Returns the delta to credit (0 on latch/resync/no-change).
  uint32_t feed(uint32_t bridgeTotal) {
    if (!synced) { last = bridgeTotal; synced = true; return 0; }
    if (bridgeTotal < last) { last = bridgeTotal; return 0; }
    uint32_t d = bridgeTotal - last;
    last = bridgeTotal;
    return d;
  }
};
