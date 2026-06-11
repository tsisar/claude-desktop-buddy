#include "../buddy.h"
#include "../buddy_common.h"
#include <string.h>


namespace penguin {

// ─── SLEEP ───  ~12s cycle, 6 poses, curled on ice
static void doSleep(uint32_t t) {
  static const char* const TUCK[5]    = { "            ", "   .---.    ", "  ( -- )    ", "  (_____)   ", "   ~~~~~    " };
  static const char* const BREATHE[5] = { "            ", "   .---.    ", "  ( -- )    ", "  (_____)   ", "   =====    " };
  static const char* const SNORE[5]   = { "    o O .   ", "   .---.    ", "  ( __ )    ", "  (_____)   ", "   =====    " };
  static const char* const TIPPED[5]  = { "            ", "            ", "  .-----.   ", " ( --   )=> ", "  `~~~~~`   " };
  static const char* const TIPPED2[5] = { "            ", "            ", "  .-----.   ", " ( zz   )=> ", "  `~~~~~`   " };
  static const char* const TWITCH[5]  = { "            ", "   .---.    ", "  ( ^^ )    ", " /(_____)   ", "   ~~~~~    " };

  const char* const* P[6] = { TUCK, BREATHE, SNORE, TIPPED, TIPPED2, TWITCH };
  static const uint8_t SEQ[] = {
    0,1,0,1,0,1,2,1,
    0,1,0,1,
    3,4,3,4,3,4,
    3,3,
    1,5,1,1
  };
  uint8_t beat = (t / 5) % sizeof(SEQ);
  buddyPrintSprite(P[SEQ[beat]], 5, 0, 0x041F);

  // Z particles drift up-right with cold-blue tint
  int p1 = (t)     % 10;
  int p2 = (t + 4) % 10;
  int p3 = (t + 7) % 10;
  buddySetColor(BUDDY_CYAN);
  buddySetCursor(BUDDY_X_CENTER + 20 + p1, BUDDY_Y_OVERLAY + 18 - p1 * 2);
  buddyPrint("z");
  buddySetColor(BUDDY_WHITE);
  buddySetCursor(BUDDY_X_CENTER + 26 + p2, BUDDY_Y_OVERLAY + 14 - p2);
  buddyPrint("Z");
  buddySetColor(BUDDY_DIM);
  buddySetCursor(BUDDY_X_CENTER + 16 + p3 / 2, BUDDY_Y_OVERLAY + 10 - p3 / 2);
  buddyPrint("z");
}

// ─── IDLE ───  ~14s cycle, 10 poses of waddling, formal posture
static void doIdle(uint32_t t) {
  static const char* const STAND[5]   = { "   .---.    ", "  ( o>o )   ", " /(     )\\  ", "  `-----`   ", "   J   L    " };
  static const char* const WAD_L[5]   = { "   .---.    ", "  ( o>o )   ", "/(     )    ", "  `-----`   ", "  J    L    " };
  static const char* const WAD_R[5]   = { "   .---.    ", "  ( o>o )   ", " (     )\\   ", "  `-----`   ", "   J    L   " };
  static const char* const BLINK[5]   = { "   .---.    ", "  ( ->- )   ", " /(     )\\  ", "  `-----`   ", "   J   L    " };
  static const char* const LOOK_L[5]  = { "   .---.    ", "  (o> o )   ", " /(     )\\  ", "  `-----`   ", "   J   L    " };
  static const char* const LOOK_R[5]  = { "   .---.    ", "  ( o >o)   ", " /(     )\\  ", "  `-----`   ", "   J   L    " };
  static const char* const PREEN[5]   = { "   .---.    ", "  ( o>o )   ", " /(  v  )\\  ", "  `-----`   ", "   J   L    " };
  static const char* const FLAP[5]    = { "  \\.---./   ", "  ( o>o )   ", "/(     )\\   ", "  `-----`   ", "   J   L    " };
  static const char* const BOW[5]     = { "            ", "   .---.    ", "  ( v>v )   ", " /(_____)\\  ", "   J   L    " };
  static const char* const STRETCH[5] = { "  /.---.\\   ", " ( ^>^ )    ", "//(     )\\\\ ", "  `-----`   ", "   J   L    " };

  const char* const* P[10] = { STAND, WAD_L, WAD_R, BLINK, LOOK_L, LOOK_R, PREEN, FLAP, BOW, STRETCH };
  static const uint8_t SEQ[] = {
    0,0,1,2,1,2,0,3,
    0,4,0,5,0,
    6,6,0,3,
    0,1,2,1,2,0,
    7,7,0,0,
    8,8,0,
    9,9,0,0
  };
  uint8_t beat = (t / 5) % sizeof(SEQ);
  buddyPrintSprite(P[SEQ[beat]], 5, 0, 0x041F);
}

// ─── BUSY ───  ~10s cycle, 6 poses + dot ticker, formal flipper-typing
static void doBusy(uint32_t t) {
  static const char* const TYPE_A[5]  = { "   .---.    ", "  ( v>v )   ", " /(     )\\  ", " /`-----`\\  ", "   J   L    " };
  static const char* const TYPE_B[5]  = { "   .---.    ", "  ( v>v )   ", " \\(     )/  ", " \\`-----`/  ", "   J   L    " };
  static const char* const THINK[5]   = { "      ?     ", "   .---.    ", "  ( ^>^ )   ", " /(  .  )\\  ", "   J   L    " };
  static const char* const SIP[5]     = { "    [_]     ", "   .---.|   ", "  ( o>o |   ", " /(     )\\  ", "   J   L    " };
  static const char* const EUREKA[5]  = { "      *     ", "   .---.    ", "  ( O>O )   ", " /(  ^  )\\  ", "   J   L    " };
  static const char* const RELIEF[5]  = { "    ~~~     ", "   .---.    ", "  ( ->- )   ", " /(  _  )\\  ", "   J   L    " };

  const char* const* P[6] = { TYPE_A, TYPE_B, THINK, SIP, EUREKA, RELIEF };
  static const uint8_t SEQ[] = {
    0,1,0,1,0,1, 2,2, 0,1,0,1, 3,3, 2,4, 0,1,0,1,5
  };
  uint8_t beat = (t / 5) % sizeof(SEQ);
  buddyPrintSprite(P[SEQ[beat]], 5, 0, 0x041F);

  overlayDots(t);
}

// ─── ATTENTION ───  ~8s cycle, 6 poses + ! pulse, alert flippers up
static void doAttention(uint32_t t) {
  static const char* const ALERT[5]   = { "   .---.    ", "  ( O>O )   ", " /(     )\\  ", "  `-----`   ", "   J   L    " };
  static const char* const SCAN_L[5]  = { "   .---.    ", "  (O> O )   ", " /(     )\\  ", "  `-----`   ", "   J   L    " };
  static const char* const SCAN_R[5]  = { "   .---.    ", "  ( O >O)   ", " /(     )\\  ", "  `-----`   ", "   J   L    " };
  static const char* const SCAN_U[5]  = { "   .---.    ", "  ( ^>^ )   ", " /(     )\\  ", "  `-----`   ", "   J   L    " };
  static const char* const TENSE[5]   = { "  /.---.\\   ", " /( O>O )\\  ", "//(     )\\\\ ", "  `-----`   ", "  J     L   " };
  static const char* const HUSH[5]    = { "   .---.    ", "  ( o>o )   ", " /(  .  )\\  ", "  `-----`   ", "   J   L    " };

  const char* const* P[6] = { ALERT, SCAN_L, SCAN_R, SCAN_U, TENSE, HUSH };
  static const uint8_t SEQ[] = {
    0,4,0,1,0,2,0,3, 4,4,0,1,2,0, 5,0
  };
  uint8_t beat = (t / 5) % sizeof(SEQ);
  uint8_t pose = SEQ[beat];
  int xOff = (pose == 4) ? ((t & 1) ? 1 : -1) : 0;
  buddyPrintSprite(P[pose], 5, 0, 0x041F, xOff);

  overlayBang(t);
}

// ─── CELEBRATE ───  ~5.6s cycle, 6 poses + confetti rain, jumping penguin
static void doCelebrate(uint32_t t) {
  static const char* const CROUCH[5]  = { "            ", "   .---.    ", "  ( ^>^ )   ", " /(_____)\\  ", "   J   L    " };
  static const char* const JUMP[5]    = { "  \\.---./   ", "  ( ^>^ )   ", " /(     )\\  ", "  `-----`   ", "   ^   ^    " };
  static const char* const PEAK[5]    = { "  \\^---^/   ", "  ( O>O )   ", " /(  W  )\\  ", "  `-----`   ", "   v   v    " };
  static const char* const SPIN_L[5]  = { "   .---.    ", "  ( <>< )   ", "/(     )    ", "  `-----`   ", "   /   \\    " };
  static const char* const SPIN_R[5]  = { "   .---.    ", "  (>< ><)   ", " (     )\\   ", "  `-----`   ", "   \\   /    " };
  static const char* const POSE[5]    = { "    \\_/     ", "   .---.    ", "  ( ^>^ )   ", "/(  W  )\\   ", "   J   L    " };

  const char* const* P[6] = { CROUCH, JUMP, PEAK, SPIN_L, SPIN_R, POSE };
  uint8_t beat = (t / 3) % sizeof(BUDDY_CELEB_SEQ);
  buddyPrintSprite(P[BUDDY_CELEB_SEQ[beat]], 5, BUDDY_CELEB_YSHIFT[beat], 0x041F);

  overlayConfetti(t);
}

// ─── DIZZY ───  ~5.6s cycle, 5 poses + orbiting stars, slipping on ice
static void doDizzy(uint32_t t) {
  static const char* const SLIP_L[5]  = { "  .---.     ", " ( @>@ )    ", "/(     )    ", " `-----`    ", "  J   L     " };
  static const char* const SLIP_R[5]  = { "    .---.   ", "   ( @>@ )  ", "   (     )\\ ", "    `-----` ", "     J   L  " };
  static const char* const WOOZY[5]   = { "   .---.    ", "  ( x>@ )   ", " /(  ~  )\\  ", "  `-----`   ", "   J   L    " };
  static const char* const WOOZY2[5]  = { "   .---.    ", "  ( @>x )   ", " /(  ~  )\\  ", "  `-----`   ", "   J   L    " };
  static const char* const SPLAT[5]   = { "            ", "   .---.    ", "  ( @>@ )   ", " (_______)= ", "   ~~~~~    " };

  const char* const* P[5] = { SLIP_L, SLIP_R, WOOZY, WOOZY2, SPLAT };
  uint8_t beat = (t / 4) % sizeof(BUDDY_DIZZY_SEQ);
  buddyPrintSprite(P[BUDDY_DIZZY_SEQ[beat]], 5, 0, 0x041F, BUDDY_DIZZY_XSHIFT[beat]);

  overlayOrbitStars(t);
}

// ─── HEART ───  ~10s cycle, 5 poses + rising heart stream, dreamy penguin
static void doHeart(uint32_t t) {
  static const char* const DREAMY[5]  = { "   .---.    ", "  ( ^>^ )   ", " /(     )\\  ", "  `-----`   ", "   J   L    " };
  static const char* const BLUSH[5]   = { "   .---.    ", "  (#^>^#)   ", " /(     )\\  ", "  `-----`   ", "   J   L    " };
  static const char* const EYES_C[5]  = { "   .---.    ", "  (<3><3)   ", " /(     )\\  ", "  `-----`   ", "   J   L    " };
  static const char* const TWIRL[5]   = { "   .---.    ", "  ( @>@ )   ", "/(     )\\   ", "  `-----`   ", "    \\ /     " };
  static const char* const SIGH[5]    = { "   .---.    ", "  ( ->- )   ", " /(  ^  )\\  ", "  `-----`   ", "   J   L    " };

  const char* const* P[5] = { DREAMY, BLUSH, EYES_C, TWIRL, SIGH };
  static const uint8_t SEQ[] = {
    0,0,1,0, 2,2,0, 1,0,4, 0,0,3,3, 0,1,0,2, 1,0
  };
  static const int8_t Y_BOB[] = { 0,-1,0,-1, 0,-1,0, -1,0,0, -1,0,0,0, -1,0,-1,0, -1,0 };
  uint8_t beat = (t / 5) % sizeof(SEQ);
  buddyPrintSprite(P[SEQ[beat]], 5, Y_BOB[beat], 0x041F);

  overlayHearts(t);
}

}  // namespace penguin

extern const Species PENGUIN_SPECIES = {
  "penguin",
  0x041F,
  { penguin::doSleep, penguin::doIdle, penguin::doBusy, penguin::doAttention,
    penguin::doCelebrate, penguin::doDizzy, penguin::doHeart }
};
