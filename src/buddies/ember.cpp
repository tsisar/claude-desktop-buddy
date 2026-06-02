#include "../buddy.h"
#include "../buddy_common.h"

// "ember" — a deliberately simple, STATIC buddy in Claude terracotta
// (#DE7643 / RGB565 0xDBA8). No animation yet: every persona state renders the
// same blob silhouette. The README art it's modelled on uses Unicode block
// glyphs (▐▛█▜) that the ASCII-only buddy font can't draw, so this is an ASCII
// silhouette of the same rounded, two-footed shape. Swap in 7 animated states
// later to bring it to life.

namespace ember {

static const uint16_t TERRACOTTA = 0xDBA8;

// rounded top → widest body → two little feet (each row is centered)
static const char* const BODY[3] = {
  ".#####.",
  "#########",
  "##   ##",
};

static void draw(uint32_t /*t*/) {
  buddyPrintSprite(BODY, 3, 8, TERRACOTTA);
}

}  // namespace ember

extern const Species EMBER_SPECIES = {
  "ember",
  ember::TERRACOTTA,
  { ember::draw, ember::draw, ember::draw, ember::draw,
    ember::draw, ember::draw, ember::draw }
};
