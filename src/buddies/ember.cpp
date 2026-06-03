#include "../buddy.h"
#include "../buddy_common.h"

// "ember" — a deliberately simple, STATIC block-art buddy in Claude
// terracotta. Doubles as the demo/reference for the blockart renderer:
// every glyph below is a Unicode Block Element (U+2580–U+259F) filled as
// seamless rectangles by buddyPrintBlocks (see src/blockart.*). All seven
// persona states render the same frame.

namespace ember {

// Claude terracotta (#D97757) in RGB565.
static const uint16_t TERRACOTTA = 0xDBAA;

static void draw(uint32_t /*t*/) {
  static const char* const POSE[3] = {
    " ▐▛███▜▌ ",
    "▝▜█████▛▘",
    "  ▘▘ ▝▝  ",
  };
  buddyPrintBlocks(POSE, 3, 0, TERRACOTTA);
}

}  // namespace ember

extern const Species EMBER_SPECIES = {
  "ember",
  ember::TERRACOTTA,
  { ember::draw, ember::draw, ember::draw, ember::draw,
    ember::draw, ember::draw, ember::draw }
};