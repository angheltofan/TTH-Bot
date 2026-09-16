// Host-side tests proving the end-of-turn report is never truncated.
//
// THE DEFECT THIS PREVENTS
//
// The single-line summary reached ~218 characters at its worst against a
// 192-byte queue slot. snprintf would have truncated it silently: memory-safe,
// but the fields most likely to be cut were `dropped`, `highWater`, `audio`
// and `drainTotal` -- precisely the ones that prove the turn completed and the
// microphone was released. A lifecycle report that loses its conclusion is
// worse than no report, because it still looks authoritative.
//
// So the report is two explicitly named lines, and these tests feed the
// formatter absurd worst-case values and assert every required field survives
// intact.

#include <string.h>

#include <unity.h>

#include "tth/CaptureSummary.h"
#include "tth/LogQueue.h"

namespace {

// Deliberately beyond anything the hardware can produce: the longest stop
// reason, a full 46-second buffer, and uint32 maxima for every counter.
tth::CaptureSummaryData worstCase() {
  tth::CaptureSummaryData data;
  data.reason = "microphone unavailable";  // the longest reason string
  data.durationMs = 4294967295u;
  data.samples = 4294967295u;
  data.bytes = 4294967295u;
  data.highWaterBytes = 4294967295u;
  data.minRms = 1.0f;
  data.averageRms = 1.0f;
  data.maxRms = 1.0f;
  data.peak = 1.0f;
  data.chunks = 4294967295u;
  data.failedReads = 4294967295u;
  data.droppedSamples = 4294967295u;
  data.audioOwner = "speaker";  // the longest owner name
  data.drainTotalMs = 4294967295u;
  return data;
}

// A realistic 45-second forced stop.
tth::CaptureSummaryData realistic() {
  tth::CaptureSummaryData data;
  data.reason = "maximum hold";
  data.durationMs = 45000;
  data.samples = 719360;
  data.bytes = 1438720;
  data.highWaterBytes = 1438720;
  data.minRms = 0.0021f;
  data.averageRms = 0.0388f;
  data.maxRms = 0.1104f;
  data.peak = 0.2431f;
  data.chunks = 1405;
  data.failedReads = 0;
  data.droppedSamples = 0;
  data.audioOwner = "none";
  data.drainTotalMs = 61;
  return data;
}

bool contains(const char* haystack, const char* needle) {
  return strstr(haystack, needle) != nullptr;
}

}  // namespace

void setUp() {}
void tearDown() {}

// --- the lines must fit -----------------------------------------------------

// snprintf returns the length the line WOULD need. Anything at or above the
// slot size means truncation.
static void both_lines_fit_a_queue_slot_at_worst_case() {
  char line[tth::kLogMessageMax];
  const tth::CaptureSummaryData data = worstCase();

  const int needed1 =
      tth::formatCaptureSummaryLine1(line, sizeof(line), data);
  TEST_ASSERT_TRUE(needed1 > 0);
  TEST_ASSERT_TRUE(static_cast<size_t>(needed1) < tth::kLogMessageMax);

  const int needed2 =
      tth::formatCaptureSummaryLine2(line, sizeof(line), data);
  TEST_ASSERT_TRUE(needed2 > 0);
  TEST_ASSERT_TRUE(static_cast<size_t>(needed2) < tth::kLogMessageMax);
}

// Worst case must also leave genuine headroom, so a later field addition does
// not silently push it over the edge.
static void both_lines_leave_headroom_at_worst_case() {
  char line[tth::kLogMessageMax];
  const tth::CaptureSummaryData data = worstCase();

  const int needed1 =
      tth::formatCaptureSummaryLine1(line, sizeof(line), data);
  const int needed2 =
      tth::formatCaptureSummaryLine2(line, sizeof(line), data);

  // At least 25 % spare in each.
  TEST_ASSERT_TRUE(static_cast<size_t>(needed1) < (tth::kLogMessageMax * 3) / 4);
  TEST_ASSERT_TRUE(static_cast<size_t>(needed2) < (tth::kLogMessageMax * 3) / 4);
}

// Going through the queue -- copy included -- must not lose anything either.
static void the_queued_lines_survive_a_round_trip_intact() {
  const tth::CaptureSummaryData data = worstCase();
  char line1[tth::kLogMessageMax];
  char line2[tth::kLogMessageMax];
  tth::formatCaptureSummaryLine1(line1, sizeof(line1), data);
  tth::formatCaptureSummaryLine2(line2, sizeof(line2), data);

  tth::LogQueue queue;
  TEST_ASSERT_TRUE(queue.pushCritical(line1));
  TEST_ASSERT_TRUE(queue.pushCritical(line2));

  char out1[tth::kLogMessageMax];
  char out2[tth::kLogMessageMax];
  TEST_ASSERT_TRUE(queue.pop(out1, sizeof(out1)));
  TEST_ASSERT_TRUE(queue.pop(out2, sizeof(out2)));

  // Byte-for-byte identical: no truncation anywhere in the path.
  TEST_ASSERT_EQUAL_STRING(line1, out1);
  TEST_ASSERT_EQUAL_STRING(line2, out2);
}

// --- every required field must be present -----------------------------------

// The explicit list from the requirement: reason, duration, samples, bytes,
// RMS min/avg/max, peak, chunks, failed, dropped, highWater, audio=none and
// drainTotal.
static void the_complete_report_preserves_every_required_field() {
  const tth::CaptureSummaryData data = realistic();
  char line1[tth::kLogMessageMax];
  char line2[tth::kLogMessageMax];
  tth::formatCaptureSummaryLine1(line1, sizeof(line1), data);
  tth::formatCaptureSummaryLine2(line2, sizeof(line2), data);

  // Line 1: what was captured.
  TEST_ASSERT_TRUE(contains(line1, "SUMMARY1"));
  TEST_ASSERT_TRUE(contains(line1, "reason=maximum hold"));
  TEST_ASSERT_TRUE(contains(line1, "duration=45000ms"));
  TEST_ASSERT_TRUE(contains(line1, "samples=719360"));
  TEST_ASSERT_TRUE(contains(line1, "bytes=1438720"));
  TEST_ASSERT_TRUE(contains(line1, "highWater=1438720B"));

  // Line 2: levels, health, and the proof the hardware was released.
  TEST_ASSERT_TRUE(contains(line2, "SUMMARY2"));
  TEST_ASSERT_TRUE(contains(line2, "rms min/avg/max=0.0021/0.0388/0.1104"));
  TEST_ASSERT_TRUE(contains(line2, "peak=0.2431"));
  TEST_ASSERT_TRUE(contains(line2, "chunks=1405"));
  TEST_ASSERT_TRUE(contains(line2, "failed=0"));
  TEST_ASSERT_TRUE(contains(line2, "dropped=0"));
  TEST_ASSERT_TRUE(contains(line2, "audio=none"));
  TEST_ASSERT_TRUE(contains(line2, "drainTotal=61ms"));
}

// ...and they must still be present at worst case, where truncation would
// otherwise bite.
static void every_required_field_survives_worst_case() {
  const tth::CaptureSummaryData data = worstCase();
  char line1[tth::kLogMessageMax];
  char line2[tth::kLogMessageMax];
  tth::formatCaptureSummaryLine1(line1, sizeof(line1), data);
  tth::formatCaptureSummaryLine2(line2, sizeof(line2), data);

  const char* line1Fields[5] = {"reason=", "duration=", "samples=", "bytes=",
                                "highWater="};
  for (int i = 0; i < 5; ++i) {
    TEST_ASSERT_TRUE(contains(line1, line1Fields[i]));
  }

  const char* line2Fields[7] = {"rms min/avg/max=", "peak=",  "chunks=",
                                "failed=",          "dropped=", "audio=",
                                "drainTotal="};
  for (int i = 0; i < 7; ++i) {
    TEST_ASSERT_TRUE(contains(line2, line2Fields[i]));
  }

  // The last field on each line must be complete, terminator and all -- that is
  // what truncation would take first.
  TEST_ASSERT_TRUE(contains(line1, "highWater=4294967295B"));
  TEST_ASSERT_TRUE(contains(line2, "drainTotal=4294967295ms"));
}

// The two halves must be distinguishable, so a missing one is obvious rather
// than silently absent.
static void the_two_lines_are_explicitly_named() {
  const tth::CaptureSummaryData data = realistic();
  char line1[tth::kLogMessageMax];
  char line2[tth::kLogMessageMax];
  tth::formatCaptureSummaryLine1(line1, sizeof(line1), data);
  tth::formatCaptureSummaryLine2(line2, sizeof(line2), data);

  TEST_ASSERT_FALSE(contains(line1, "SUMMARY2"));
  TEST_ASSERT_FALSE(contains(line2, "SUMMARY1"));
}

// --- the report cannot be crowded out ---------------------------------------

// Routine chatter must not be able to consume the slots a lifecycle report
// needs.
static void routine_diagnostics_cannot_crowd_out_the_report() {
  tth::LogQueue queue;

  // Saturate with ordinary diagnostics.
  for (int i = 0; i < 1000; ++i) queue.push("chatter");

  TEST_ASSERT_TRUE(queue.drops() > 0);
  TEST_ASSERT_EQUAL_UINT32(0, queue.criticalDrops());
  // Routine pushes stopped short of the reserve.
  TEST_ASSERT_EQUAL_UINT32(queue.capacity() - tth::kLogReservedCriticalSlots,
                           queue.size());

  // Both halves of the report still fit.
  const tth::CaptureSummaryData data = realistic();
  char line1[tth::kLogMessageMax];
  char line2[tth::kLogMessageMax];
  tth::formatCaptureSummaryLine1(line1, sizeof(line1), data);
  tth::formatCaptureSummaryLine2(line2, sizeof(line2), data);

  TEST_ASSERT_TRUE(queue.pushCritical(line1));
  TEST_ASSERT_TRUE(queue.pushCritical(line2));
  TEST_ASSERT_EQUAL_UINT32(0, queue.criticalDrops());
}

// A genuinely full queue still refuses, but counts it separately so a lost
// lifecycle line is never mistaken for ordinary diagnostic loss.
static void a_lost_critical_line_is_counted_separately() {
  tth::LogQueue queue;
  for (size_t i = 0; i < queue.capacity(); ++i) queue.pushCritical("critical");

  TEST_ASSERT_TRUE(queue.isFull());
  TEST_ASSERT_FALSE(queue.pushCritical("one too many"));
  TEST_ASSERT_EQUAL_UINT32(1, queue.criticalDrops());
  TEST_ASSERT_EQUAL_UINT32(0, queue.drops());
}

// --- Phase 5: the two-line playback summary -----------------------------------

#include "tth/PlaybackSummary.h"

static tth::PlaybackSummaryData worstCasePlayback() {
  tth::PlaybackSummaryData d;
  d.end = "cancelled";
  d.rate = 4294967295u;
  d.queued = 4294967295u;
  d.played = 4294967295u;
  d.samples = 4294967295u;
  d.underruns = 4294967295u;
  d.rejects = 4294967295u;
  d.refusals = 4294967295u;
  d.audioOwner = "speaker";
  d.drainMs = 4294967295u;
  d.source = "synthetic";
  d.gainDb = -120.0f;
  d.inputPeak = 1000.0f;
  d.outputPeak = 1000.0f;
  d.limitedSamples = 4294967295u;
  d.maxLevel = 1000.0f;
  d.masterVolume = 4294967295u;
  d.pathGainDb = -120.0f;
  return d;
}

// A lifecycle report is never allowed to truncate.
static void both_playback_summary_lines_fit_at_worst_case() {
  const tth::PlaybackSummaryData d = worstCasePlayback();
  char line[512];
  const int n1 = tth::formatPlaybackSummaryLine1(line, sizeof(line), d);
  const int n2 = tth::formatPlaybackSummaryLine2(line, sizeof(line), d);
  TEST_ASSERT_TRUE(n1 > 0 && n1 < static_cast<int>(tth::kLogMessageMax));
  TEST_ASSERT_TRUE(n2 > 0 && n2 < static_cast<int>(tth::kLogMessageMax));
}

static void the_playback_summary_carries_every_loudness_field() {
  tth::PlaybackSummaryData d = worstCasePlayback();
  d.end = "completed";
  d.rate = 16000;
  d.audioOwner = "none";
  d.source = "loopback";
  d.gainDb = 9.0f;
  d.inputPeak = 0.125f;
  d.outputPeak = 0.352f;
  d.limitedSamples = 12;
  d.maxLevel = 0.2f;
  d.masterVolume = 255;
  d.pathGainDb = -0.1f;

  char line[tth::kLogMessageMax];
  tth::formatPlaybackSummaryLine1(line, sizeof(line), d);
  TEST_ASSERT_NOT_NULL(strstr(line, "[play] SUMMARY1 end=completed rate=16000Hz"));
  TEST_ASSERT_NOT_NULL(strstr(line, "underruns="));
  TEST_ASSERT_NOT_NULL(strstr(line, "audio=none"));
  TEST_ASSERT_NOT_NULL(strstr(line, "drain="));

  tth::formatPlaybackSummaryLine2(line, sizeof(line), d);
  TEST_ASSERT_NOT_NULL(strstr(line, "[play] SUMMARY2 source=loopback"));
  TEST_ASSERT_NOT_NULL(strstr(line, "gainDb=+9.0"));
  TEST_ASSERT_NOT_NULL(strstr(line, "inputPeak=0.125"));
  TEST_ASSERT_NOT_NULL(strstr(line, "outputPeak=0.352"));
  TEST_ASSERT_NOT_NULL(strstr(line, "limitedSamples=12"));
  TEST_ASSERT_NOT_NULL(strstr(line, "maxLevel=0.200"));
  TEST_ASSERT_NOT_NULL(strstr(line, "master=255"));
  TEST_ASSERT_NOT_NULL(strstr(line, "pathGainDb=-0.1"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(both_playback_summary_lines_fit_at_worst_case);
  RUN_TEST(the_playback_summary_carries_every_loudness_field);
  RUN_TEST(both_lines_fit_a_queue_slot_at_worst_case);
  RUN_TEST(both_lines_leave_headroom_at_worst_case);
  RUN_TEST(the_queued_lines_survive_a_round_trip_intact);
  RUN_TEST(the_complete_report_preserves_every_required_field);
  RUN_TEST(every_required_field_survives_worst_case);
  RUN_TEST(the_two_lines_are_explicitly_named);
  RUN_TEST(routine_diagnostics_cannot_crowd_out_the_report);
  RUN_TEST(a_lost_critical_line_is_counted_separately);
  return UNITY_END();
}
