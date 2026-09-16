// Host-side tests for TurnStreamer: live streaming of the user turn out of
// the PSRAM TurnBuffer through a read cursor, with lossless backpressure.
//
// The two TurnBuffer invariants are pinned here from the reader's side:
//   * only committed samples are ever read;
//   * the buffer cannot be reset while unconsumed audio remains.

#include <string.h>

#include <vector>

#include <unity.h>

#include "tth/AudioFormat.h"
#include "tth/ITurnSource.h"
#include "tth/TurnBuffer.h"
#include "tth/TurnStreamer.h"

namespace {

const uint32_t kCapacity = 4000;
const uint32_t kFrame = 320;
const uint32_t kMaxFrames = 2;

int16_t g_storage[kCapacity];

// A turn source that behaves the way the interface demands: it COPIES each
// pushed chunk before returning (the pointer is only borrowed for the call),
// and it checks every chunk against the buffer's committed length at the
// moment of the push.
class RecordingSink : public tth::ITurnSource {
 public:
  const tth::TurnBuffer* buffer = nullptr;

  bool beginResult = true;
  tth::PushResult nextResult = tth::PushResult::Accepted;

  std::vector<int16_t> received;
  std::vector<const int16_t*> pointers;
  uint32_t pushes = 0;
  uint32_t busyReturned = 0;
  uint32_t begins = 0;
  uint32_t ends = 0;
  uint32_t uncommittedReads = 0;
  uint32_t outsideBuffer = 0;
  // Samples delivered before endUserTurn() -- must equal the total.
  uint32_t samplesAtEnd = 0;

  bool beginUserTurn(const tth::AudioFormat& format) override {
    (void)format;
    ++begins;
    return beginResult;
  }

  tth::PushResult pushUserAudio(const tth::AudioChunk& chunk) override {
    ++pushes;
    if (nextResult != tth::PushResult::Accepted) {
      if (nextResult == tth::PushResult::Busy) ++busyReturned;
      return nextResult;
    }
    // Committed-length boundary, observed from outside.
    const int16_t* base = buffer->data();
    if (chunk.samples < base ||
        chunk.samples + chunk.count > base + buffer->capacity()) {
      ++outsideBuffer;
    }
    if (chunk.samples + chunk.count > base + buffer->committedSamples()) {
      ++uncommittedReads;
    }
    pointers.push_back(chunk.samples);
    // Copy before returning: the pointer is borrowed.
    received.insert(received.end(), chunk.samples,
                    chunk.samples + chunk.count);
    return tth::PushResult::Accepted;
  }

  void endUserTurn() override {
    ++ends;
    samplesAtEnd = static_cast<uint32_t>(received.size());
  }
  void cancel() override {}
  void poll(uint32_t) override {}
  bool nextEvent(tth::TurnEvent&) override { return false; }
  bool peekPlaybackChunk(tth::AudioChunk&, uint32_t) override { return false; }
  void consumePlayback(uint32_t) override {}
};

// Appends a ramp continuing from `first`, so every sample value is its own
// position in the turn -- loss, duplication and gaps are all detectable.
uint32_t appendRamp(tth::TurnBuffer& buffer, uint32_t first, uint32_t count) {
  std::vector<int16_t> chunk(count);
  for (uint32_t i = 0; i < count; ++i) {
    chunk[i] = static_cast<int16_t>(first + i);
  }
  return buffer.append(chunk.data(), count);
}

void assertExactRamp(const std::vector<int16_t>& received, uint32_t count) {
  TEST_ASSERT_EQUAL_UINT32(count, static_cast<uint32_t>(received.size()));
  for (uint32_t i = 0; i < count; ++i) {
    if (received[i] != static_cast<int16_t>(i)) {
      TEST_FAIL_MESSAGE("received audio has a gap, duplicate or reordering");
    }
  }
}

struct Harness {
  tth::TurnBuffer buffer;
  RecordingSink sink;
  tth::TurnStreamer streamer;

  Harness() : streamer(kFrame, kMaxFrames) {
    memset(g_storage, 0, sizeof(g_storage));
    buffer.attach(g_storage, kCapacity);
    sink.buffer = &buffer;
  }

  bool begin() {
    return streamer.begin(buffer, sink, tth::monoS16(16000));
  }
};

}  // namespace

void setUp() {}
void tearDown() {}

// Live streaming: audio leaves while the turn is still being recorded, not
// after it has finished.
static void audio_is_streamed_while_still_recording() {
  Harness h;
  TEST_ASSERT_TRUE(h.begin());

  appendRamp(h.buffer, 0, 2 * kFrame);
  TEST_ASSERT_TRUE(h.streamer.service(false) == tth::StreamStatus::Streaming);

  TEST_ASSERT_EQUAL_UINT32(2 * kFrame, h.streamer.cursor());
  TEST_ASSERT_EQUAL_UINT32(2 * kFrame,
                           static_cast<uint32_t>(h.sink.received.size()));
  // The user turn is NOT over yet.
  TEST_ASSERT_EQUAL_UINT32(0, h.sink.ends);
}

// Committed-length boundary: a remainder shorter than a frame is not sent
// while the recording is live, and nothing past the committed length is ever
// read.
static void only_committed_samples_are_ever_read() {
  Harness h;
  TEST_ASSERT_TRUE(h.begin());

  appendRamp(h.buffer, 0, kFrame + 100);
  for (int i = 0; i < 5; ++i) h.streamer.service(false);

  TEST_ASSERT_EQUAL_UINT32(kFrame, h.streamer.cursor());
  TEST_ASSERT_EQUAL_UINT32(0, h.sink.uncommittedReads);

  // Once the recording is over, the short tail goes too.
  h.streamer.service(true);
  TEST_ASSERT_EQUAL_UINT32(kFrame + 100, h.streamer.cursor());
  TEST_ASSERT_EQUAL_UINT32(0, h.sink.uncommittedReads);
  assertExactRamp(h.sink.received, kFrame + 100);
}

// THE DELIBERATE BACKPRESSURE TEST.
//
// Busy must cost a retry and never a sample: the cursor holds, the recording
// carries on filling the buffer, and when Busy clears the exact pending
// samples are delivered, in order, with no loss, duplication or gap.
static void busy_holds_the_cursor_and_loses_nothing() {
  Harness h;
  TEST_ASSERT_TRUE(h.begin());

  // Some audio goes through normally first.
  appendRamp(h.buffer, 0, kFrame);
  h.streamer.service(false);
  TEST_ASSERT_EQUAL_UINT32(kFrame, h.streamer.cursor());
  const uint32_t deliveredBeforeBusy =
      static_cast<uint32_t>(h.sink.received.size());

  // Force Busy.
  h.sink.nextResult = tth::PushResult::Busy;
  uint32_t recorded = kFrame;
  for (int i = 0; i < 6; ++i) {
    recorded += appendRamp(h.buffer, recorded, kFrame);
    const uint32_t before = h.streamer.cursor();
    h.streamer.service(false);
    // 1. The cursor does not advance while Busy.
    TEST_ASSERT_EQUAL_UINT32(before, h.streamer.cursor());
  }
  TEST_ASSERT_EQUAL_UINT32(kFrame, h.streamer.cursor());
  TEST_ASSERT_EQUAL_UINT32(deliveredBeforeBusy,
                           static_cast<uint32_t>(h.sink.received.size()));
  TEST_ASSERT_EQUAL_UINT32(6, h.streamer.metrics().busyRetries);
  TEST_ASSERT_EQUAL_UINT32(6 * kFrame, h.streamer.lagSamples());

  // The recording ends while the source is still busy: the lease must hold
  // the unsent tail in place.
  TEST_ASSERT_FALSE(h.streamer.service(true) == tth::StreamStatus::Finished);
  TEST_ASSERT_TRUE(h.buffer.isRetained());
  TEST_ASSERT_FALSE(h.buffer.reset());

  // 2. Remove Busy.
  h.sink.nextResult = tth::PushResult::Accepted;
  const size_t firstAfterBusy = h.sink.pointers.size();
  int guard = 0;
  while (h.streamer.service(true) == tth::StreamStatus::Streaming) {
    TEST_ASSERT_TRUE(++guard < 100);
  }

  // 3. The first chunk after Busy is exactly the one that was refused.
  TEST_ASSERT_EQUAL_PTR(h.buffer.data() + kFrame,
                        h.sink.pointers[firstAfterBusy]);
  // 4. Every sample, once, in order: no loss, no duplicate, no gap.
  assertExactRamp(h.sink.received, recorded);
  TEST_ASSERT_EQUAL_UINT32(recorded, h.sink.samplesAtEnd);
  TEST_ASSERT_EQUAL_UINT32(1, h.sink.ends);
  TEST_ASSERT_EQUAL_UINT32(6 * kFrame, h.streamer.metrics().maxLagSamples);
}

// Backpressure that comes and goes, several times, mid-recording.
static void intermittent_busy_still_delivers_every_sample_once() {
  Harness h;
  TEST_ASSERT_TRUE(h.begin());

  uint32_t recorded = 0;
  for (int i = 0; i < 10; ++i) {
    recorded += appendRamp(h.buffer, recorded, 250);
    h.sink.nextResult =
        (i % 3 == 1) ? tth::PushResult::Busy : tth::PushResult::Accepted;
    h.streamer.service(false);
  }
  h.sink.nextResult = tth::PushResult::Accepted;
  int guard = 0;
  while (h.streamer.service(true) == tth::StreamStatus::Streaming) {
    TEST_ASSERT_TRUE(++guard < 100);
  }

  assertExactRamp(h.sink.received, recorded);
  TEST_ASSERT_TRUE(h.streamer.metrics().busyRetries > 0);
}

// No second buffer: every pushed pointer aliases the turn buffer itself.
static void nothing_is_copied_into_a_second_buffer() {
  Harness h;
  TEST_ASSERT_TRUE(h.begin());
  appendRamp(h.buffer, 0, 1000);
  while (h.streamer.service(true) == tth::StreamStatus::Streaming) {
  }

  TEST_ASSERT_TRUE(h.sink.pointers.size() > 0);
  TEST_ASSERT_EQUAL_UINT32(0, h.sink.outsideBuffer);
  uint32_t expected = 0;
  for (size_t i = 0; i < h.sink.pointers.size(); ++i) {
    TEST_ASSERT_EQUAL_PTR(h.buffer.data() + expected, h.sink.pointers[i]);
    expected += (i + 1 < h.sink.pointers.size()) ? kFrame : 0;
  }
}

// Lifetime: the lease is held from begin() until the last committed sample
// has been accepted -- including after the recording itself has stopped.
static void the_buffer_cannot_be_reset_until_the_tail_is_sent() {
  Harness h;
  TEST_ASSERT_TRUE(h.begin());
  TEST_ASSERT_TRUE(h.buffer.isRetained());

  appendRamp(h.buffer, 0, 5 * kFrame);
  // Recording over, but only kMaxFrames go per call.
  TEST_ASSERT_TRUE(h.streamer.service(true) == tth::StreamStatus::Streaming);
  TEST_ASSERT_TRUE(h.buffer.isRetained());
  TEST_ASSERT_FALSE(h.buffer.reset());
  TEST_ASSERT_EQUAL_UINT32(5 * kFrame, h.buffer.committedSamples());

  while (h.streamer.service(true) == tth::StreamStatus::Streaming) {
  }
  TEST_ASSERT_TRUE(h.streamer.status() == tth::StreamStatus::Finished);
  TEST_ASSERT_FALSE(h.buffer.isRetained());
  TEST_ASSERT_TRUE(h.buffer.reset());
}

static void end_of_turn_is_sent_once_and_only_after_the_last_sample() {
  Harness h;
  TEST_ASSERT_TRUE(h.begin());
  appendRamp(h.buffer, 0, 3 * kFrame + 7);

  while (h.streamer.service(true) == tth::StreamStatus::Streaming) {
  }
  h.streamer.service(true);
  h.streamer.service(true);

  TEST_ASSERT_EQUAL_UINT32(1, h.sink.ends);
  TEST_ASSERT_EQUAL_UINT32(3 * kFrame + 7, h.sink.samplesAtEnd);
}

static void one_service_call_sends_at_most_the_frame_budget() {
  Harness h;
  TEST_ASSERT_TRUE(h.begin());
  appendRamp(h.buffer, 0, 10 * kFrame);

  h.streamer.service(false);
  TEST_ASSERT_EQUAL_UINT32(kMaxFrames, h.sink.pushes);
  TEST_ASSERT_EQUAL_UINT32(kMaxFrames * kFrame, h.streamer.cursor());
}

static void fatal_fails_the_stream_and_releases_the_lease() {
  Harness h;
  TEST_ASSERT_TRUE(h.begin());
  appendRamp(h.buffer, 0, kFrame);
  h.sink.nextResult = tth::PushResult::Fatal;

  TEST_ASSERT_TRUE(h.streamer.service(false) == tth::StreamStatus::Failed);
  TEST_ASSERT_FALSE(h.streamer.isActive());
  TEST_ASSERT_FALSE(h.buffer.isRetained());
  TEST_ASSERT_EQUAL_UINT32(0, h.sink.ends);
}

static void a_refused_begin_holds_nothing() {
  Harness h;
  h.sink.beginResult = false;

  TEST_ASSERT_FALSE(h.begin());
  TEST_ASSERT_FALSE(h.streamer.isActive());
  TEST_ASSERT_FALSE(h.buffer.isRetained());
}

static void a_second_begin_while_streaming_is_refused() {
  Harness h;
  TEST_ASSERT_TRUE(h.begin());
  TEST_ASSERT_FALSE(h.begin());
  TEST_ASSERT_EQUAL_UINT32(1, h.buffer.readers());
}

static void abandon_releases_the_lease_without_ending_the_turn() {
  Harness h;
  TEST_ASSERT_TRUE(h.begin());
  appendRamp(h.buffer, 0, kFrame);
  h.streamer.service(false);

  h.streamer.abandon();
  TEST_ASSERT_FALSE(h.streamer.isActive());
  TEST_ASSERT_FALSE(h.buffer.isRetained());
  TEST_ASSERT_EQUAL_UINT32(0, h.sink.ends);
}

static void an_empty_turn_ends_immediately() {
  Harness h;
  TEST_ASSERT_TRUE(h.begin());
  TEST_ASSERT_TRUE(h.streamer.service(true) == tth::StreamStatus::Finished);
  TEST_ASSERT_EQUAL_UINT32(1, h.sink.ends);
  TEST_ASSERT_EQUAL_UINT32(0, h.sink.pushes);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(audio_is_streamed_while_still_recording);
  RUN_TEST(only_committed_samples_are_ever_read);
  RUN_TEST(busy_holds_the_cursor_and_loses_nothing);
  RUN_TEST(intermittent_busy_still_delivers_every_sample_once);
  RUN_TEST(nothing_is_copied_into_a_second_buffer);
  RUN_TEST(the_buffer_cannot_be_reset_until_the_tail_is_sent);
  RUN_TEST(end_of_turn_is_sent_once_and_only_after_the_last_sample);
  RUN_TEST(one_service_call_sends_at_most_the_frame_budget);
  RUN_TEST(fatal_fails_the_stream_and_releases_the_lease);
  RUN_TEST(a_refused_begin_holds_nothing);
  RUN_TEST(a_second_begin_while_streaming_is_refused);
  RUN_TEST(abandon_releases_the_lease_without_ending_the_turn);
  RUN_TEST(an_empty_turn_ends_immediately);
  return UNITY_END();
}
