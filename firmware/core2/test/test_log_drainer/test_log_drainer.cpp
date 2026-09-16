// Host-side tests for the resumable partial log writer.
//
// THE DEFECT THESE PIN: head-of-line blocking
//
// The first drain refused to write a message unless the whole line fitted in
// the UART's free space. The ESP32 TX FIFO is 128 bytes, so
// availableForWrite() never reports more -- and SUMMARY1/SUMMARY2 need 130 and
// 162 bytes including CRLF. Neither could EVER be written. They stuck at the
// head of the queue forever and every later message starved behind them: the
// summaries never appeared, and the [cap], [mem] and [blocks] lines stopped
// too.
//
// The tests below run the drainer against sinks that report one byte free,
// zero bytes free, and a different number every call, and assert that a
// 160-byte critical line still comes out byte-for-byte, in order, with exactly
// one newline, and that no single call exceeds its budget.

#include <string.h>

#include <string>

#include <unity.h>

#include "tth/LogDrainer.h"
#include "tth/LogQueue.h"

namespace {

const size_t kBudget = 64;

// A UART whose free space the test controls exactly.
class FakeSink : public tth::ILogSink {
 public:
  std::string written;
  size_t fixedAvailable = 1000;
  // When set, availability cycles through this pattern instead.
  const size_t* pattern = nullptr;
  size_t patternLength = 0;
  size_t patternIndex = 0;
  // Largest single write() length seen, to prove the budget holds.
  size_t largestWrite = 0;
  int writeCalls = 0;
  int availabilityCalls = 0;

  size_t availableForWrite() override {
    ++availabilityCalls;
    if (pattern != nullptr && patternLength > 0) {
      const size_t value = pattern[patternIndex % patternLength];
      ++patternIndex;
      return value;
    }
    return fixedAvailable;
  }

  size_t write(const uint8_t* data, size_t length) override {
    ++writeCalls;
    if (length > largestWrite) largestWrite = length;
    written.append(reinterpret_cast<const char*>(data), length);
    return length;
  }
};

std::string makeMessage(size_t length, char fill) {
  return std::string(length, fill);
}

size_t countOccurrences(const std::string& haystack, const std::string& needle) {
  size_t count = 0;
  size_t at = haystack.find(needle);
  while (at != std::string::npos) {
    ++count;
    at = haystack.find(needle, at + needle.size());
  }
  return count;
}

}  // namespace

void setUp() {}
void tearDown() {}

// --- the exact failure that was observed ------------------------------------

// A 160-byte message against a sink that NEVER reports enough room for the
// whole line. Under the old rule this was never written at all.
static void a_long_message_drains_when_the_sink_never_fits_it() {
  tth::LogQueue queue;
  tth::LogDrainer drainer(queue, kBudget);
  FakeSink sink;
  // Smaller than the message, permanently -- the real ESP32 FIFO behaviour.
  sink.fixedAvailable = 100;

  const std::string message = makeMessage(160, 'A');
  TEST_ASSERT_TRUE(queue.pushCritical(message.c_str()));

  for (int i = 0; i < 100 && !queue.isEmpty(); ++i) drainer.service(sink);

  TEST_ASSERT_TRUE(queue.isEmpty());
  TEST_ASSERT_EQUAL_STRING((message + "\r\n").c_str(), sink.written.c_str());
}

// The pathological case: one byte of room at a time.
static void one_byte_availability_still_drains_the_message() {
  tth::LogQueue queue;
  tth::LogDrainer drainer(queue, kBudget);
  FakeSink sink;
  sink.fixedAvailable = 1;

  const std::string message = makeMessage(160, 'B');
  queue.pushCritical(message.c_str());

  // 162 bytes at one per call, plus slack.
  for (int i = 0; i < 400 && !queue.isEmpty(); ++i) drainer.service(sink);

  TEST_ASSERT_TRUE(queue.isEmpty());
  TEST_ASSERT_EQUAL_STRING((message + "\r\n").c_str(), sink.written.c_str());
  TEST_ASSERT_EQUAL_UINT32(1, sink.largestWrite);
}

// Zero room must be a cheap no-op that changes nothing.
static void zero_availability_writes_nothing_and_loses_nothing() {
  tth::LogQueue queue;
  tth::LogDrainer drainer(queue, kBudget);
  FakeSink sink;
  sink.fixedAvailable = 0;

  queue.pushCritical("hello");
  for (int i = 0; i < 50; ++i) {
    TEST_ASSERT_EQUAL_UINT32(0, drainer.service(sink));
  }

  TEST_ASSERT_EQUAL_UINT32(1, queue.size());
  TEST_ASSERT_EQUAL_UINT32(0, sink.written.size());
  TEST_ASSERT_EQUAL_INT(0, sink.writeCalls);

  // ...and it still drains once room appears.
  sink.fixedAvailable = 1000;
  drainer.service(sink);
  TEST_ASSERT_TRUE(queue.isEmpty());
  TEST_ASSERT_EQUAL_STRING("hello\r\n", sink.written.c_str());
}

// Availability that changes every call, including zero.
static void varying_availability_drains_correctly() {
  tth::LogQueue queue;
  tth::LogDrainer drainer(queue, kBudget);
  FakeSink sink;
  static const size_t pattern[7] = {0, 1, 7, 0, 33, 2, 100};
  sink.pattern = pattern;
  sink.patternLength = 7;

  const std::string message = makeMessage(160, 'C');
  queue.pushCritical(message.c_str());

  for (int i = 0; i < 500 && !queue.isEmpty(); ++i) drainer.service(sink);

  TEST_ASSERT_TRUE(queue.isEmpty());
  TEST_ASSERT_EQUAL_STRING((message + "\r\n").c_str(), sink.written.c_str());
}

// --- ordering and completeness ----------------------------------------------

// The message behind a long one must come out afterwards, not be starved.
static void the_next_message_is_emitted_after_a_long_one() {
  tth::LogQueue queue;
  tth::LogDrainer drainer(queue, kBudget);
  FakeSink sink;
  sink.fixedAvailable = 20;  // never enough for the first line

  const std::string first = makeMessage(160, 'X');
  queue.pushCritical(first.c_str());
  queue.pushCritical("SECOND");

  for (int i = 0; i < 200 && !queue.isEmpty(); ++i) drainer.service(sink);

  TEST_ASSERT_TRUE(queue.isEmpty());
  const std::string expected = first + "\r\n" + "SECOND\r\n";
  TEST_ASSERT_EQUAL_STRING(expected.c_str(), sink.written.c_str());
}

// Bytes from two messages must never interleave.
static void ordering_is_preserved_across_many_messages() {
  tth::LogQueue queue;
  tth::LogDrainer drainer(queue, kBudget);
  FakeSink sink;
  sink.fixedAvailable = 3;  // forces every message to be split

  std::string expected;
  for (int i = 0; i < 10; ++i) {
    char name[32];
    snprintf(name, sizeof(name), "msg%02d-%s", i, "payload");
    queue.push(name);
    expected += name;
    expected += "\r\n";
  }

  for (int i = 0; i < 2000 && !queue.isEmpty(); ++i) drainer.service(sink);

  TEST_ASSERT_TRUE(queue.isEmpty());
  TEST_ASSERT_EQUAL_STRING(expected.c_str(), sink.written.c_str());
}

// Exactly one newline per message, however many pieces it took.
static void the_newline_is_emitted_exactly_once_per_message() {
  tth::LogQueue queue;
  tth::LogDrainer drainer(queue, kBudget);
  FakeSink sink;
  sink.fixedAvailable = 1;  // splits the CRLF itself across calls

  queue.pushCritical(makeMessage(40, 'D').c_str());
  queue.pushCritical(makeMessage(40, 'E').c_str());

  for (int i = 0; i < 500 && !queue.isEmpty(); ++i) drainer.service(sink);

  TEST_ASSERT_EQUAL_UINT32(2, countOccurrences(sink.written, "\r\n"));
  TEST_ASSERT_EQUAL_UINT32(84, sink.written.size());  // 2 * (40 + 2)
}

// --- the budget -------------------------------------------------------------

// No single call may exceed its budget, however much room the sink reports.
static void no_service_call_exceeds_the_byte_budget() {
  tth::LogQueue queue;
  tth::LogDrainer drainer(queue, kBudget);
  FakeSink sink;
  sink.fixedAvailable = 100000;  // unlimited room

  for (int i = 0; i < 10; ++i) queue.push(makeMessage(180, 'F').c_str());

  for (int i = 0; i < 200 && !queue.isEmpty(); ++i) {
    const size_t written = drainer.service(sink);
    TEST_ASSERT_TRUE(written <= kBudget);
  }
  TEST_ASSERT_TRUE(queue.isEmpty());
}

// A budget larger than the message must not overrun it.
static void a_generous_budget_does_not_overrun_a_short_message() {
  tth::LogQueue queue;
  tth::LogDrainer drainer(queue, 1000);
  FakeSink sink;

  queue.push("short");
  const size_t written = drainer.service(sink);

  TEST_ASSERT_EQUAL_UINT32(7, written);  // "short" + CRLF
  TEST_ASSERT_EQUAL_STRING("short\r\n", sink.written.c_str());
}

// --- partial messages are protected -----------------------------------------

// A half-written message must stay at the head, untouched, until it completes.
static void a_partial_message_is_not_dropped_or_overwritten() {
  tth::LogQueue queue;
  tth::LogDrainer drainer(queue, kBudget);
  FakeSink sink;
  sink.fixedAvailable = 10;

  const std::string message = makeMessage(160, 'G');
  queue.pushCritical(message.c_str());

  drainer.service(sink);  // partial
  TEST_ASSERT_TRUE(drainer.hasPartialMessage());
  TEST_ASSERT_EQUAL_UINT32(1, queue.size());

  // Flood the queue while the head is half-written.
  for (int i = 0; i < 100; ++i) queue.push("noise");
  const size_t queuedAfterFlood = queue.size();
  TEST_ASSERT_TRUE(queuedAfterFlood > 1);

  // Drain until the original message has been retired.
  const std::string expectedFirst = message + "\r\n";
  for (int i = 0; i < 500 && queue.size() >= queuedAfterFlood; ++i) {
    drainer.service(sink);
  }

  // It came out first, whole and uncorrupted, despite the flood arriving
  // mid-write. Later messages follow it rather than displacing it.
  TEST_ASSERT_TRUE(sink.written.size() >= expectedFirst.size());
  TEST_ASSERT_EQUAL_STRING(
      expectedFirst.c_str(), sink.written.substr(0, expectedFirst.size()).c_str());
}

// A sink that reports room but accepts nothing must not spin or lose data.
static void a_sink_that_accepts_nothing_does_not_spin() {
  class StubbornSink : public tth::ILogSink {
   public:
    size_t availableForWrite() override { return 64; }
    size_t write(const uint8_t*, size_t) override { return 0; }
  };

  tth::LogQueue queue;
  tth::LogDrainer drainer(queue, kBudget);
  StubbornSink sink;

  queue.pushCritical("message");
  for (int i = 0; i < 10; ++i) {
    TEST_ASSERT_EQUAL_UINT32(0, drainer.service(sink));
  }
  // Nothing written, nothing lost, offset untouched.
  TEST_ASSERT_EQUAL_UINT32(1, queue.size());
  TEST_ASSERT_FALSE(drainer.hasPartialMessage());
}

static void an_empty_queue_is_a_cheap_no_op() {
  tth::LogQueue queue;
  tth::LogDrainer drainer(queue, kBudget);
  FakeSink sink;

  TEST_ASSERT_EQUAL_UINT32(0, drainer.service(sink));
  TEST_ASSERT_EQUAL_INT(0, sink.writeCalls);
}

// The full realistic case: both summary lines at their true lengths, through a
// 128-byte FIFO, exactly as on the device.
static void both_summary_lines_drain_through_a_128_byte_fifo() {
  tth::LogQueue queue;
  tth::LogDrainer drainer(queue, kBudget);
  FakeSink sink;
  sink.fixedAvailable = 128;  // the ESP32 UART TX FIFO

  const std::string summary1 = makeMessage(128, '1');
  const std::string summary2 = makeMessage(160, '2');
  TEST_ASSERT_TRUE(queue.pushCritical(summary1.c_str()));
  TEST_ASSERT_TRUE(queue.pushCritical(summary2.c_str()));

  for (int i = 0; i < 200 && !queue.isEmpty(); ++i) drainer.service(sink);

  TEST_ASSERT_TRUE(queue.isEmpty());
  const std::string expected = summary1 + "\r\n" + summary2 + "\r\n";
  TEST_ASSERT_EQUAL_STRING(expected.c_str(), sink.written.c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(a_long_message_drains_when_the_sink_never_fits_it);
  RUN_TEST(one_byte_availability_still_drains_the_message);
  RUN_TEST(zero_availability_writes_nothing_and_loses_nothing);
  RUN_TEST(varying_availability_drains_correctly);
  RUN_TEST(the_next_message_is_emitted_after_a_long_one);
  RUN_TEST(ordering_is_preserved_across_many_messages);
  RUN_TEST(the_newline_is_emitted_exactly_once_per_message);
  RUN_TEST(no_service_call_exceeds_the_byte_budget);
  RUN_TEST(a_generous_budget_does_not_overrun_a_short_message);
  RUN_TEST(a_partial_message_is_not_dropped_or_overwritten);
  RUN_TEST(a_sink_that_accepts_nothing_does_not_spin);
  RUN_TEST(an_empty_queue_is_a_cheap_no_op);
  RUN_TEST(both_summary_lines_drain_through_a_128_byte_fifo);
  return UNITY_END();
}
