// Host-side tests for the diagnostic queue and the turn-report ordering.
//
// TWO DEFECTS THESE PIN
//
// 1. Diagnostics became the fault they were measuring. A [block] line on every
//    face render, plus a 250-character summary and heartbeat written
//    synchronously at 115200 baud, pushed loop iterations to 57 ms. The queue
//    below is bounded and lossy on purpose: a dropped log line is an
//    inconvenience, a blocked loop costs audio.
//
// 2. The summary was printed on button release -- before the microphone had
//    drained and before AudioBus released -- so it claimed `audio=mic` and was
//    followed by `[mic] stopped`. It described a state that had not happened.

#include <string.h>

#include <unity.h>

#include "tth/LogQueue.h"
#include "tth/TurnReportGate.h"

void setUp() {}
void tearDown() {}

// --- the bounded queue ------------------------------------------------------

static void a_new_queue_is_empty() {
  tth::LogQueue queue;
  char out[tth::kLogMessageMax];

  TEST_ASSERT_TRUE(queue.isEmpty());
  TEST_ASSERT_FALSE(queue.isFull());
  TEST_ASSERT_EQUAL_UINT32(0, queue.drops());
  TEST_ASSERT_NULL(queue.peek());
  TEST_ASSERT_FALSE(queue.pop(out, sizeof(out)));
}

static void messages_come_back_in_order() {
  tth::LogQueue queue;
  char out[tth::kLogMessageMax];

  TEST_ASSERT_TRUE(queue.push("first"));
  TEST_ASSERT_TRUE(queue.push("second"));
  TEST_ASSERT_TRUE(queue.push("third"));
  TEST_ASSERT_EQUAL_UINT32(3, queue.size());

  TEST_ASSERT_TRUE(queue.pop(out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("first", out);
  TEST_ASSERT_TRUE(queue.pop(out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("second", out);
  TEST_ASSERT_TRUE(queue.pop(out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("third", out);
  TEST_ASSERT_TRUE(queue.isEmpty());
}

// THE point of the design: bounded, never growing.
static void the_queue_is_bounded_and_counts_drops() {
  tth::LogQueue queue;
  const size_t routineLimit =
      queue.capacity() - tth::kLogReservedCriticalSlots;

  // Routine diagnostics stop at the reserve, not at the capacity: the last
  // few slots belong to lifecycle reports.
  for (size_t i = 0; i < routineLimit; ++i) {
    TEST_ASSERT_TRUE(queue.push("message"));
  }
  TEST_ASSERT_EQUAL_UINT32(routineLimit, queue.size());
  TEST_ASSERT_FALSE(queue.isFull());

  // Saturate it hard.
  for (int i = 0; i < 500; ++i) {
    TEST_ASSERT_FALSE(queue.push("overflow"));
  }

  // Size never grows past the routine limit, and every loss is counted.
  TEST_ASSERT_EQUAL_UINT32(routineLimit, queue.size());
  TEST_ASSERT_EQUAL_UINT32(500, queue.drops());
  TEST_ASSERT_EQUAL_UINT32(0, queue.criticalDrops());

  // ...and the reserve is still there for a report.
  for (size_t i = 0; i < tth::kLogReservedCriticalSlots; ++i) {
    TEST_ASSERT_TRUE(queue.pushCritical("report"));
  }
  TEST_ASSERT_TRUE(queue.isFull());
}

// Under saturation the OLDEST messages survive: during a burst the first lines
// explain what happened, the tail is repetition.
static void saturation_keeps_the_oldest_messages() {
  tth::LogQueue queue;
  char out[tth::kLogMessageMax];

  const size_t routineLimit =
      queue.capacity() - tth::kLogReservedCriticalSlots;
  queue.push("keep me");
  for (size_t i = 1; i < routineLimit; ++i) queue.push("filler");
  queue.push("dropped");

  TEST_ASSERT_EQUAL_UINT32(1, queue.drops());
  TEST_ASSERT_TRUE(queue.pop(out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("keep me", out);
}

// A saturated queue must never corrupt memory or grow a message beyond the
// slot: this is what makes it safe to call from the capture path.
static void long_messages_are_truncated_not_overflowed() {
  tth::LogQueue queue;
  char huge[tth::kLogMessageMax * 3];
  memset(huge, 'x', sizeof(huge));
  huge[sizeof(huge) - 1] = '\0';

  TEST_ASSERT_TRUE(queue.push(huge));

  char out[tth::kLogMessageMax];
  TEST_ASSERT_TRUE(queue.pop(out, sizeof(out)));
  TEST_ASSERT_EQUAL_UINT32(tth::kLogMessageMax - 1, strlen(out));
}

// Saturating the queue must be cheap and total: it may never refuse to return,
// loop, or alter anything a caller depends on.
static void saturation_cannot_block_or_corrupt_the_queue() {
  tth::LogQueue queue;

  for (int i = 0; i < 10000; ++i) queue.push("flood");
  TEST_ASSERT_EQUAL_UINT32(queue.capacity() - tth::kLogReservedCriticalSlots,
                           queue.size());

  // Still fully functional afterwards.
  char out[tth::kLogMessageMax];
  while (queue.pop(out, sizeof(out))) {
  }
  TEST_ASSERT_TRUE(queue.isEmpty());

  queue.clearDrops();
  TEST_ASSERT_EQUAL_UINT32(0, queue.drops());
  TEST_ASSERT_TRUE(queue.push("works again"));
  TEST_ASSERT_TRUE(queue.pop(out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("works again", out);
}

static void peek_does_not_remove() {
  tth::LogQueue queue;
  queue.push("one");

  TEST_ASSERT_EQUAL_STRING("one", queue.peek());
  TEST_ASSERT_EQUAL_UINT32(1, queue.size());
  queue.discardFront();
  TEST_ASSERT_TRUE(queue.isEmpty());
  TEST_ASSERT_NULL(queue.peek());
}

// --- report ordering --------------------------------------------------------

// The reported defect: the summary must NOT be emitted while the capture
// controller still owns the microphone.
static void no_summary_until_the_hardware_is_released() {
  tth::TurnReportGate gate;
  gate.onStart();

  // Nothing due while recording.
  TEST_ASSERT_FALSE(gate.takeSummaryDue(true));

  gate.onStopRequested();

  // Still nothing due while draining -- the bus still reads `mic` here.
  TEST_ASSERT_FALSE(gate.takeSummaryDue(true));
  TEST_ASSERT_FALSE(gate.takeSummaryDue(true));
  TEST_ASSERT_FALSE(gate.takeSummaryDue(true));

  // Released: due exactly now.
  TEST_ASSERT_TRUE(gate.takeSummaryDue(false));
}

static void the_summary_is_emitted_exactly_once() {
  tth::TurnReportGate gate;
  gate.onStart();
  gate.onStopRequested();

  TEST_ASSERT_TRUE(gate.takeSummaryDue(false));
  // Every later poll must stay silent.
  for (int i = 0; i < 20; ++i) {
    TEST_ASSERT_FALSE(gate.takeSummaryDue(false));
  }
}

// The forced-stop log printed another 45000 ms status line after its own
// summary. Status must stop the moment a stop is requested.
static void no_status_after_a_stop_is_requested() {
  tth::TurnReportGate gate;
  gate.onStart();
  TEST_ASSERT_TRUE(gate.shouldPrintStatus());

  gate.onStopRequested();
  TEST_ASSERT_FALSE(gate.shouldPrintStatus());

  // ...including through the whole drain, and after it.
  gate.takeSummaryDue(true);
  TEST_ASSERT_FALSE(gate.shouldPrintStatus());
  gate.takeSummaryDue(false);
  TEST_ASSERT_FALSE(gate.shouldPrintStatus());
}

static void status_is_silent_before_any_turn() {
  tth::TurnReportGate gate;
  TEST_ASSERT_FALSE(gate.shouldPrintStatus());
  TEST_ASSERT_FALSE(gate.takeSummaryDue(false));
  TEST_ASSERT_FALSE(gate.takeSummaryDue(true));
}

static void a_new_turn_re_arms_status() {
  tth::TurnReportGate gate;
  gate.onStart();
  gate.onStopRequested();
  TEST_ASSERT_TRUE(gate.takeSummaryDue(false));
  TEST_ASSERT_FALSE(gate.shouldPrintStatus());

  gate.onStart();
  TEST_ASSERT_TRUE(gate.shouldPrintStatus());
  TEST_ASSERT_FALSE(gate.isStopRequested());
}

static void several_turns_each_report_once() {
  tth::TurnReportGate gate;

  for (int turn = 0; turn < 5; ++turn) {
    gate.onStart();
    TEST_ASSERT_TRUE(gate.shouldPrintStatus());
    TEST_ASSERT_FALSE(gate.takeSummaryDue(true));

    gate.onStopRequested();
    TEST_ASSERT_FALSE(gate.shouldPrintStatus());
    TEST_ASSERT_FALSE(gate.takeSummaryDue(true));
    TEST_ASSERT_TRUE(gate.takeSummaryDue(false));
    TEST_ASSERT_FALSE(gate.takeSummaryDue(false));
  }
}

// A stop request that never got a start must not fabricate a summary.
static void a_stop_without_a_start_reports_nothing() {
  tth::TurnReportGate gate;
  gate.onStopRequested();
  TEST_ASSERT_FALSE(gate.takeSummaryDue(false));
  TEST_ASSERT_FALSE(gate.shouldPrintStatus());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(a_new_queue_is_empty);
  RUN_TEST(messages_come_back_in_order);
  RUN_TEST(the_queue_is_bounded_and_counts_drops);
  RUN_TEST(saturation_keeps_the_oldest_messages);
  RUN_TEST(long_messages_are_truncated_not_overflowed);
  RUN_TEST(saturation_cannot_block_or_corrupt_the_queue);
  RUN_TEST(peek_does_not_remove);
  RUN_TEST(no_summary_until_the_hardware_is_released);
  RUN_TEST(the_summary_is_emitted_exactly_once);
  RUN_TEST(no_status_after_a_stop_is_requested);
  RUN_TEST(status_is_silent_before_any_turn);
  RUN_TEST(a_new_turn_re_arms_status);
  RUN_TEST(several_turns_each_report_once);
  RUN_TEST(a_stop_without_a_start_reports_nothing);
  return UNITY_END();
}
