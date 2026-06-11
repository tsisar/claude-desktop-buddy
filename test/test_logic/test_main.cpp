// Host-side unit tests for the pure logic in src/logic/.
//   pio test -e native
#include <unity.h>
#include <string.h>

#include "../../src/logic/sanitize.h"
#include "../../src/logic/wrap.h"
#include "../../src/logic/translit.h"
#include "../../src/logic/token_ledger.h"
#include "../../src/logic/classify.h"

void setUp() {}
void tearDown() {}

// ── buddySafeName: the only gate between wire names and FS paths ──────────

static void test_safename_accepts_plain_names() {
  TEST_ASSERT_TRUE(buddySafeName("bufo", 24));
  TEST_ASSERT_TRUE(buddySafeName("pack_v2.1-final", 24));
  TEST_ASSERT_TRUE(buddySafeName("A", 24));
}

static void test_safename_rejects_traversal_and_separators() {
  TEST_ASSERT_FALSE(buddySafeName("..", 24));
  TEST_ASSERT_FALSE(buddySafeName(".", 24));
  TEST_ASSERT_FALSE(buddySafeName("../etc", 24));
  TEST_ASSERT_FALSE(buddySafeName("a/b", 24));
  TEST_ASSERT_FALSE(buddySafeName("/abs", 24));
  TEST_ASSERT_FALSE(buddySafeName("a\\b", 24));
}

static void test_safename_rejects_empty_null_charset_length() {
  TEST_ASSERT_FALSE(buddySafeName("", 24));
  TEST_ASSERT_FALSE(buddySafeName(nullptr, 24));
  TEST_ASSERT_FALSE(buddySafeName("sp ace", 24));
  TEST_ASSERT_FALSE(buddySafeName("emoji\xF0\x9F\x98\x80", 24));
  TEST_ASSERT_FALSE(buddySafeName("12345678", 8));   // n == maxLen → too long
  TEST_ASSERT_TRUE(buddySafeName("1234567", 8));     // n == maxLen-1 fits
  // "..." is three dots — allowed charset, not the ".." special case.
  TEST_ASSERT_TRUE(buddySafeName("...", 24));
}

// ── wrapInto: HUD / transcript / approval hint wrapping ───────────────────

static void test_wrap_simple_fit() {
  char out[4][48];
  uint8_t n = wrapInto("hello world", out, 4, 20);
  TEST_ASSERT_EQUAL_UINT8(1, n);
  TEST_ASSERT_EQUAL_STRING("hello world", out[0]);
}

static void test_wrap_breaks_on_word_boundary() {
  char out[4][48];
  uint8_t n = wrapInto("aaaa bbbb cccc", out, 4, 9);
  TEST_ASSERT_EQUAL_UINT8(2, n);
  TEST_ASSERT_EQUAL_STRING("aaaa bbbb", out[0]);
  // Continuation rows carry a one-space indent.
  TEST_ASSERT_EQUAL_STRING(" cccc", out[1]);
}

static void test_wrap_hard_splits_long_word() {
  char out[4][48];
  uint8_t n = wrapInto("abcdefghijkl", out, 4, 5);
  // Long-standing quirk this test pins down: a first word too long for the
  // row closes (empty) row 0, then hard-splits into indented rows of
  // width-1 chars.
  TEST_ASSERT_EQUAL_UINT8(4, n);
  TEST_ASSERT_EQUAL_STRING("", out[0]);
  TEST_ASSERT_EQUAL_STRING(" abcd", out[1]);
  TEST_ASSERT_EQUAL_STRING(" efgh", out[2]);
  TEST_ASSERT_EQUAL_STRING(" ijkl", out[3]);
}

static void test_wrap_respects_utf8_boundary() {
  char out[4][48];
  // "ééééé" = 10 bytes of 2-byte chars at width 6: the hard-split take of
  // 5 bytes would cut a char in half — the back-off must snap it to 4.
  uint8_t n = wrapInto("\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9", out, 4, 6);
  TEST_ASSERT_EQUAL_UINT8(4, n);
  TEST_ASSERT_EQUAL_UINT8(5, (uint8_t)strlen(out[1]));            // ' ' + 2 whole chars
  TEST_ASSERT_EQUAL_HEX8(0xC3, (unsigned char)out[1][1]);         // row starts on a lead byte
  TEST_ASSERT_EQUAL_HEX8(0xA9, (unsigned char)out[1][4]);         // ...and ends on a complete char
  TEST_ASSERT_EQUAL_STRING(" \xC3\xA9", out[3]);                  // remainder intact
}

static void test_wrap_row_cap() {
  char out[2][48];
  uint8_t n = wrapInto("one two three four five six seven eight", out, 2, 5);
  TEST_ASSERT_EQUAL_UINT8(2, n);
}

// ── translit / asciiCopy: UTF-8 → 6x8 font charset ────────────────────────

static void test_ascii_passthrough_and_control_strip() {
  char b[32];
  asciiCopy(b, sizeof(b), "ok\x01\x02 fine\n");
  TEST_ASSERT_EQUAL_STRING("ok fine", b);
}

static void test_cyrillic_romanised() {
  char b[32];
  // Привіт — the table maps и→i (RU mapping), so this comes out "Privit".
  asciiCopy(b, sizeof(b), "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD1\x96\xD1\x82");
  TEST_ASSERT_EQUAL_STRING("Privit", b);
}

static void test_punctuation_mapped_emoji_replaced() {
  char b[32];
  // a—b 😀 …   (string split after \x94: a hex escape would greedily eat
  // the following 'b' as a hex digit)
  asciiCopy(b, sizeof(b), "a\xE2\x80\x94" "b \xF0\x9F\x98\x80 \xE2\x80\xA6");
  TEST_ASSERT_EQUAL_STRING("a-b ? ...", b);
}

static void test_truncated_utf8_no_overrun() {
  char b[8];
  // Lead byte with missing continuation at end of string must not read past.
  asciiCopy(b, sizeof(b), "ab\xD0");
  TEST_ASSERT_EQUAL_STRING("ab?", b);
  // Stray continuation byte is skipped.
  asciiCopy(b, sizeof(b), "\x80xy");
  TEST_ASSERT_EQUAL_STRING("xy", b);
}

static void test_asciicopy_respects_dst_len() {
  char b[4];
  asciiCopy(b, sizeof(b), "abcdef");
  TEST_ASSERT_EQUAL_STRING("abc", b);
}

// ── TokenLedger: first-sight latch + bridge-restart resync ────────────────

static void test_ledger_first_sight_latches() {
  TokenLedger l;
  TEST_ASSERT_EQUAL_UINT32(0, l.feed(500000));   // device reboot: no re-credit
  TEST_ASSERT_EQUAL_UINT32(100, l.feed(500100)); // then deltas flow
}

static void test_ledger_bridge_restart_resyncs() {
  TokenLedger l;
  l.feed(1000);
  TEST_ASSERT_EQUAL_UINT32(200, l.feed(1200));
  TEST_ASSERT_EQUAL_UINT32(0, l.feed(50));       // total dropped: resync only
  TEST_ASSERT_EQUAL_UINT32(25, l.feed(75));
}

static void test_ledger_no_change_no_credit() {
  TokenLedger l;
  l.feed(10);
  TEST_ASSERT_EQUAL_UINT32(0, l.feed(10));
}

// ── classifyGesture: the approve-vs-deny decision ──────────────────────────

static void test_classify_tap() {
  TEST_ASSERT_EQUAL(GESTURE_TAP, classifyGesture(0, 0, 50));
  TEST_ASSERT_EQUAL(GESTURE_TAP, classifyGesture(14, -14, 299));
}

static void test_classify_tap_edges_fail_to_none() {
  TEST_ASSERT_EQUAL(GESTURE_NONE, classifyGesture(15, 0, 50));    // radius edge
  TEST_ASSERT_EQUAL(GESTURE_NONE, classifyGesture(0, 0, 300));    // time edge
}

static void test_classify_swipes() {
  TEST_ASSERT_EQUAL(GESTURE_SWIPE_RIGHT, classifyGesture(120, 10, 200));
  TEST_ASSERT_EQUAL(GESTURE_SWIPE_LEFT,  classifyGesture(-80, 0, 200));
  TEST_ASSERT_EQUAL(GESTURE_SWIPE_DOWN,  classifyGesture(10, 120, 200));
  TEST_ASSERT_EQUAL(GESTURE_SWIPE_UP,    classifyGesture(0, -80, 200));
}

static void test_classify_diagonal_dominant_axis() {
  TEST_ASSERT_EQUAL(GESTURE_SWIPE_RIGHT, classifyGesture(100, 90, 200));
  // Perfect diagonal: neither axis dominates — dropped.
  TEST_ASSERT_EQUAL(GESTURE_NONE, classifyGesture(100, 100, 200));
}

static void test_classify_slow_short_move_dropped() {
  TEST_ASSERT_EQUAL(GESTURE_NONE, classifyGesture(40, 5, 500));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_safename_accepts_plain_names);
  RUN_TEST(test_safename_rejects_traversal_and_separators);
  RUN_TEST(test_safename_rejects_empty_null_charset_length);
  RUN_TEST(test_wrap_simple_fit);
  RUN_TEST(test_wrap_breaks_on_word_boundary);
  RUN_TEST(test_wrap_hard_splits_long_word);
  RUN_TEST(test_wrap_respects_utf8_boundary);
  RUN_TEST(test_wrap_row_cap);
  RUN_TEST(test_ascii_passthrough_and_control_strip);
  RUN_TEST(test_cyrillic_romanised);
  RUN_TEST(test_punctuation_mapped_emoji_replaced);
  RUN_TEST(test_truncated_utf8_no_overrun);
  RUN_TEST(test_asciicopy_respects_dst_len);
  RUN_TEST(test_ledger_first_sight_latches);
  RUN_TEST(test_ledger_bridge_restart_resyncs);
  RUN_TEST(test_ledger_no_change_no_credit);
  RUN_TEST(test_classify_tap);
  RUN_TEST(test_classify_tap_edges_fail_to_none);
  RUN_TEST(test_classify_swipes);
  RUN_TEST(test_classify_diagonal_dominant_axis);
  RUN_TEST(test_classify_slow_short_move_dropped);
  return UNITY_END();
}
