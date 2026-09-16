// Host-side tests for SerialLineAssembler: diagnostic keys vs '!' lines,
// bounded input, timeouts and wiping.

#include <string>

#include <unity.h>

#include "tth/SerialLineAssembler.h"

using tth::SerialEvent;
using tth::SerialEventType;
using tth::SerialLineAssembler;

void setUp() {}
void tearDown() {}

namespace {

// Feeds `text`; returns the last non-None event.
SerialEvent feedAll(SerialLineAssembler& a, const std::string& text, uint32_t nowMs) {
  SerialEvent last;
  last.type = SerialEventType::None;
  last.key = 0;
  last.line = nullptr;
  last.length = 0;
  for (char c : text) {
    const SerialEvent e = a.feed(static_cast<uint8_t>(c), nowMs);
    if (e.type != SerialEventType::None) last = e;
  }
  return last;
}

}  // namespace

static void single_keys_pass_straight_through() {
  SerialLineAssembler a;
  const SerialEvent e = a.feed('r', 0);
  TEST_ASSERT_TRUE(e.type == SerialEventType::Key);
  TEST_ASSERT_EQUAL_INT('r', e.key);
  TEST_ASSERT_TRUE(a.feed('\n', 0).type == SerialEventType::None);
  TEST_ASSERT_TRUE(a.feed('\r', 0).type == SerialEventType::None);
  TEST_ASSERT_FALSE(a.inLine());
}

// Typing "prov" must not fire the 'p' key (or any other).
static void a_bang_line_is_buffered_not_acted_on() {
  SerialLineAssembler a;
  TEST_ASSERT_TRUE(a.feed('!', 0).type == SerialEventType::None);
  TEST_ASSERT_TRUE(a.inLine());
  for (char c : std::string("prov show")) {
    TEST_ASSERT_TRUE(a.feed(static_cast<uint8_t>(c), 0).type == SerialEventType::None);
  }
  const SerialEvent e = a.feed('\r', 0);
  TEST_ASSERT_TRUE(e.type == SerialEventType::Line);
  TEST_ASSERT_EQUAL_UINT32(9, e.length);
  TEST_ASSERT_EQUAL_STRING("prov show", e.line);
  // The LF of CRLF is swallowed, and keys work again.
  TEST_ASSERT_TRUE(a.feed('\n', 0).type == SerialEventType::None);
  TEST_ASSERT_TRUE(a.feed('m', 0).type == SerialEventType::Key);
}

static void spaces_and_bangs_inside_a_line_are_literal() {
  SerialLineAssembler a;
  const SerialEvent e = feedAll(a, "!prov set ssid  a!b \n", 0);
  TEST_ASSERT_TRUE(e.type == SerialEventType::Line);
  TEST_ASSERT_EQUAL_STRING("prov set ssid  a!b ", e.line);
}

static void an_empty_line_is_still_a_line() {
  SerialLineAssembler a;
  const SerialEvent e = feedAll(a, "!\n", 0);
  TEST_ASSERT_TRUE(e.type == SerialEventType::Line);
  TEST_ASSERT_EQUAL_UINT32(0, e.length);
}

static void a_line_of_exactly_the_maximum_is_accepted() {
  SerialLineAssembler a;
  const std::string body(SerialLineAssembler::kMaxLine, 'x');
  const SerialEvent e = feedAll(a, "!" + body + "\n", 0);
  TEST_ASSERT_TRUE(e.type == SerialEventType::Line);
  TEST_ASSERT_EQUAL_UINT32(SerialLineAssembler::kMaxLine, e.length);
}

static void an_oversized_line_is_reported_once_and_discarded() {
  SerialLineAssembler a;
  int overflows = 0;
  int lines = 0;
  int keys = 0;
  const std::string input =
      "!" + std::string(SerialLineAssembler::kMaxLine + 300, 'p') + "\n";
  for (char c : input) {
    const SerialEvent e = a.feed(static_cast<uint8_t>(c), 0);
    if (e.type == SerialEventType::Overflow) ++overflows;
    if (e.type == SerialEventType::Line) ++lines;
    if (e.type == SerialEventType::Key) ++keys;
  }
  TEST_ASSERT_EQUAL_INT(1, overflows);
  TEST_ASSERT_EQUAL_INT(0, lines);
  TEST_ASSERT_EQUAL_INT(0, keys);  // the discarded bytes never become keys
  TEST_ASSERT_TRUE(a.isWiped());
  TEST_ASSERT_FALSE(a.inLine());
  TEST_ASSERT_TRUE(a.feed('r', 0).type == SerialEventType::Key);
}

static void an_abandoned_line_times_out_back_to_keys() {
  SerialLineAssembler a;
  feedAll(a, "!prov set pass secret", 1000);
  a.poll(1000 + SerialLineAssembler::kIdleTimeoutMs - 1);
  TEST_ASSERT_TRUE(a.inLine());
  a.poll(1000 + SerialLineAssembler::kIdleTimeoutMs);
  TEST_ASSERT_FALSE(a.inLine());
  TEST_ASSERT_TRUE(a.isWiped());
  TEST_ASSERT_TRUE(a.feed('r', 40000).type == SerialEventType::Key);
}

static void wipe_zeroes_the_buffer() {
  SerialLineAssembler a;
  const SerialEvent e = feedAll(a, "!prov set token abc\n", 0);
  TEST_ASSERT_TRUE(e.type == SerialEventType::Line);
  TEST_ASSERT_FALSE(a.isWiped());
  a.wipe();
  TEST_ASSERT_TRUE(a.isWiped());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(single_keys_pass_straight_through);
  RUN_TEST(a_bang_line_is_buffered_not_acted_on);
  RUN_TEST(spaces_and_bangs_inside_a_line_are_literal);
  RUN_TEST(an_empty_line_is_still_a_line);
  RUN_TEST(a_line_of_exactly_the_maximum_is_accepted);
  RUN_TEST(an_oversized_line_is_reported_once_and_discarded);
  RUN_TEST(an_abandoned_line_times_out_back_to_keys);
  RUN_TEST(wipe_zeroes_the_buffer);
  return UNITY_END();
}
