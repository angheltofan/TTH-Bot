// Host-side tests for the loopback ownership model.
//
// ITurnSource::pushUserAudio() borrows its pointer for that call ONLY.
// LocalMockTurnSource keeps nothing of it -- just a count. Loopback replays
// through its OWN lease on the TurnBuffer, held from beginUserTurn() until the
// player has copied the last sample out, or the turn is cancelled.
//
// These tests pin both halves of that:
//   * no borrowed AudioChunk -- the object or the memory it points into -- is
//     relied on after pushUserAudio() returns;
//   * the TurnBuffer cannot be reset (and a capture cannot start over it)
//     while any component still depends on its samples -- including the whole
//     WAITING "think" delay after the streamer has already let go.

#include <string.h>

#include <deque>
#include <vector>

#include <unity.h>

#include "tth/AudioBus.h"
#include "tth/AudioFormat.h"
#include "tth/CaptureController.h"
#include "tth/IAudioCapture.h"
#include "tth/ITurnSource.h"
#include "tth/LocalMockTurnSource.h"
#include "tth/PcmPlayer.h"
#include "tth/TurnBuffer.h"
#include "tth/TurnStreamer.h"

namespace {

const uint32_t kCapacity = 4000;
const uint32_t kTurn = 2000;
const uint32_t kFrame = 320;
const uint32_t kThinkMs = 600;
const uint32_t kSlot = 960;
const uint32_t kStart = 1000;

int16_t g_storage[kCapacity];
int16_t g_scratch[kSlot];
int16_t g_slots[3][kSlot];
int16_t g_captureChunk[512];

tth::MockConfig config() {
  tth::MockConfig c;
  c.captureFormat = tth::monoS16(16000);
  c.assistantFormat = tth::monoS16(24000);
  c.thinkMs = kThinkMs;
  c.synthDurationMs = 100;
  c.busyWindowMs = 0;
  c.busyPeriodMs = 0;
  return c;
}

// Distinct, recognisable values: sample i of the user's turn.
int16_t userSample(uint32_t i) {
  return static_cast<int16_t>((i * 7u + 3u) & 0x7FFFu);
}

void recordInto(tth::TurnBuffer& buffer, uint32_t count) {
  std::vector<int16_t> ramp(count);
  for (uint32_t i = 0; i < count; ++i) ramp[i] = userSample(i);
  buffer.append(ramp.data(), count);
}

tth::AudioChunk viewOf(const tth::TurnBuffer& buffer, uint32_t offset,
                       uint32_t count) {
  tth::AudioChunk chunk;
  chunk.samples = buffer.data() + offset;
  chunk.count = count;
  chunk.format = tth::monoS16(16000);
  return chunk;
}

class NullDevice : public tth::IAudioDevice {
 public:
  bool micBegin() override { return true; }
  void micEnd() override {}
  bool speakerBegin() override { return true; }
  void speakerEnd() override {}
};

class HoldingSpeaker : public tth::ISpeakerOutput {
 public:
  std::deque<const int16_t*> held;
  uint32_t slotsOccupied() const override {
    return static_cast<uint32_t>(held.size());
  }
  bool play(const int16_t* samples, uint32_t, uint32_t) override {
    if (held.size() >= tth::kSpeakerQueueDepth) {
      TEST_FAIL_MESSAGE("play() with both speaker slots occupied");
    }
    held.push_back(samples);
    return true;
  }
  void stop() override {}
};

class SilentCapture : public tth::IAudioCapture {
 public:
  bool startCapture() override { return true; }
  int readChunk(int16_t*, uint32_t) override { return 0; }
  void requestStop() override {}
  bool isDrained() const override { return true; }
  void finishStop() override {}
  uint32_t maxReadMicros() const override { return 0; }
  void resetReadStats() override {}
};

struct Harness {
  tth::TurnBuffer buffer;
  tth::LocalMockTurnSource mock;
  tth::TurnStreamer streamer;

  Harness() : mock(buffer, config()), streamer(kFrame, 2) {
    memset(g_storage, 0, sizeof(g_storage));
    buffer.attach(g_storage, kCapacity);
    mock.attachSynthScratch(g_scratch, kSlot);
    mock.poll(kStart);
  }

  // Records the user turn while streaming it, the way the device does, until
  // the streamer has finished and let go of its own lease.
  void recordAndStream() {
    TEST_ASSERT_TRUE(streamer.begin(buffer, mock, tth::monoS16(16000)));
    std::vector<int16_t> ramp(kTurn);
    for (uint32_t i = 0; i < kTurn; ++i) ramp[i] = userSample(i);
    for (uint32_t done = 0; done < kTurn; done += 250) {
      buffer.append(ramp.data() + done, 250);
      streamer.service(false);
    }
    int guard = 0;
    while (streamer.service(true) == tth::StreamStatus::Streaming) {
      TEST_ASSERT_TRUE(++guard < 100);
    }
    TEST_ASSERT_TRUE(streamer.status() == tth::StreamStatus::Finished);
  }

  void startResponse() {
    mock.poll(kStart + kThinkMs);
    tth::TurnEvent event;
    TEST_ASSERT_TRUE(mock.nextEvent(event));
    TEST_ASSERT_TRUE(event.type == tth::TurnEventType::SpeechStart);
  }
};

}  // namespace

void setUp() {}
void tearDown() {}

// --- 1. Nothing borrowed is kept ---------------------------------------------

// Every chunk object handed to pushUserAudio() is scribbled over the moment
// the call returns -- pointer, count and format. Replay is still exact,
// because the mock never kept the chunk: it re-derives every address from the
// buffer it holds a lease on.
static void no_borrowed_chunk_is_relied_on_after_push_returns() {
  Harness h;
  recordInto(h.buffer, kTurn);
  TEST_ASSERT_TRUE(h.mock.beginUserTurn(tth::monoS16(16000)));

  for (uint32_t offset = 0; offset < kTurn; offset += kFrame) {
    const uint32_t count = (kTurn - offset < kFrame) ? kTurn - offset : kFrame;
    tth::AudioChunk borrowed = viewOf(h.buffer, offset, count);
    TEST_ASSERT_TRUE(h.mock.pushUserAudio(borrowed) ==
                     tth::PushResult::Accepted);
    // The loan is over. Destroy what was lent.
    memset(&borrowed, 0xA5, sizeof(borrowed));
  }
  h.mock.endUserTurn();
  h.startResponse();
  TEST_ASSERT_EQUAL_UINT32(kTurn, h.mock.responseSamples());

  uint32_t cursor = 0;
  tth::AudioChunk chunk;
  while (h.mock.peekPlaybackChunk(chunk, kSlot)) {
    // From the leased buffer, not from anything that was pushed.
    TEST_ASSERT_EQUAL_PTR(h.buffer.data() + cursor, chunk.samples);
    for (uint32_t i = 0; i < chunk.count; ++i) {
      TEST_ASSERT_EQUAL_INT16(userSample(cursor + i), chunk.samples[i]);
    }
    cursor += chunk.count;
    h.mock.consumePlayback(chunk.count);
  }
  TEST_ASSERT_EQUAL_UINT32(kTurn, cursor);
}

// The mock depends on NO memory outside its lease: it accepts a chunk only if
// it is exactly the next committed range of the buffer it leases. Anything it
// could not re-derive from that buffer later is refused, so there is nothing
// borrowed it would ever need to keep.
static void only_the_next_committed_range_of_the_leased_buffer_is_accepted() {
  Harness h;
  recordInto(h.buffer, 2 * kFrame);
  TEST_ASSERT_TRUE(h.mock.beginUserTurn(tth::monoS16(16000)));

  // An IDENTICAL copy of the right samples, at another address: refused.
  int16_t copy[kFrame];
  memcpy(copy, h.buffer.data(), sizeof(copy));
  tth::AudioChunk elsewhere = viewOf(h.buffer, 0, kFrame);
  elsewhere.samples = copy;
  TEST_ASSERT_TRUE(h.mock.pushUserAudio(elsewhere) == tth::PushResult::Fatal);

  // Samples not yet committed: refused.
  TEST_ASSERT_TRUE(h.mock.pushUserAudio(viewOf(h.buffer, 0, 3 * kFrame)) ==
                   tth::PushResult::Fatal);
  TEST_ASSERT_EQUAL_UINT32(0, h.mock.receivedSamples());

  // The right range: accepted.
  TEST_ASSERT_TRUE(h.mock.pushUserAudio(viewOf(h.buffer, 0, kFrame)) ==
                   tth::PushResult::Accepted);

  // The same range again (a duplicate): refused.
  TEST_ASSERT_TRUE(h.mock.pushUserAudio(viewOf(h.buffer, 0, kFrame)) ==
                   tth::PushResult::Fatal);
  // Skipping ahead (a gap): refused.
  TEST_ASSERT_TRUE(h.mock.pushUserAudio(viewOf(h.buffer, kFrame + 10, 100)) ==
                   tth::PushResult::Fatal);

  // The next contiguous range is still accepted.
  TEST_ASSERT_TRUE(h.mock.pushUserAudio(viewOf(h.buffer, kFrame, kFrame)) ==
                   tth::PushResult::Accepted);
  TEST_ASSERT_EQUAL_UINT32(2 * kFrame, h.mock.receivedSamples());
  TEST_ASSERT_EQUAL_UINT32(4, h.mock.continuityErrors());
}

// --- 2. The buffer is held for as long as anything depends on it ---------------

// From the first sample to the last copy -- with no gap when the streamer lets
// go -- and the player's copies stay valid after the buffer is then reused.
static void the_buffer_is_protected_from_first_sample_to_last_copy() {
  Harness h;

  TEST_ASSERT_TRUE(h.streamer.begin(h.buffer, h.mock, tth::monoS16(16000)));
  // Two readers: the streamer, and loopback's own lease for the replay.
  TEST_ASSERT_EQUAL_UINT32(2, h.buffer.readers());
  TEST_ASSERT_TRUE(h.mock.holdsLease());
  h.streamer.abandon();  // restart through the full helper below
  h.mock.cancel();

  h.recordAndStream();

  // The streamer has let go. Loopback has NOT: it still needs every sample.
  TEST_ASSERT_EQUAL_UINT32(1, h.buffer.readers());
  TEST_ASSERT_TRUE(h.mock.holdsLease());
  TEST_ASSERT_FALSE(h.buffer.reset());
  TEST_ASSERT_EQUAL_UINT32(kTurn, h.mock.receivedSamples());

  // The whole WAITING think delay is covered.
  h.mock.poll(kStart + kThinkMs - 1);
  TEST_ASSERT_FALSE(h.buffer.reset());
  h.startResponse();
  TEST_ASSERT_FALSE(h.buffer.reset());

  NullDevice device;
  tth::AudioBus bus(device);
  HoldingSpeaker speaker;
  tth::PcmPlayer player(bus, speaker, 400);
  player.begin(g_slots[0], g_slots[1], g_slots[2], kSlot);
  TEST_ASSERT_TRUE(player.openStream(tth::monoS16(16000), 0));

  uint32_t cursor = 0;
  uint32_t lastChunkStart = 0;
  uint32_t lastChunkCount = 0;
  tth::AudioChunk chunk;
  while (h.mock.peekPlaybackChunk(chunk, player.slotSamples())) {
    // Still depended on until the last sample has been copied out.
    TEST_ASSERT_TRUE(h.mock.holdsLease());
    TEST_ASSERT_FALSE(h.buffer.reset());

    uint32_t accepted = 0;
    TEST_ASSERT_TRUE(player.submit(chunk, accepted) ==
                     tth::PlayerPush::Accepted);
    lastChunkStart = cursor;
    lastChunkCount = accepted;
    h.mock.consumePlayback(accepted);
    cursor += accepted;
    player.service(cursor);
    if (speaker.held.size() > 1) speaker.held.pop_front();
  }
  TEST_ASSERT_EQUAL_UINT32(kTurn, cursor);

  // Every sample copied out: nothing depends on the buffer any more.
  TEST_ASSERT_FALSE(h.mock.holdsLease());
  TEST_ASSERT_EQUAL_UINT32(0, h.buffer.readers());
  TEST_ASSERT_TRUE(h.buffer.reset());

  // Reuse the buffer for a new recording. What the speaker is playing is the
  // player's own copy, and is unaffected.
  std::vector<int16_t> next(kCapacity, static_cast<int16_t>(0x7777));
  h.buffer.append(next.data(), kCapacity);
  TEST_ASSERT_TRUE(speaker.held.size() >= 1);
  const int16_t* playing = speaker.held.back();
  for (uint32_t i = 0; i < lastChunkCount; ++i) {
    TEST_ASSERT_EQUAL_INT16(userSample(lastChunkStart + i), playing[i]);
  }
}

// End to end: a new capture cannot start over audio loopback still has to
// play -- during the think delay (after the streamer has finished) and during
// the response -- and can the moment the last sample is copied out.
static void a_capture_cannot_start_while_loopback_still_depends_on_the_audio() {
  NullDevice device;
  tth::AudioBus bus(device);
  SilentCapture microphone;
  tth::CaptureController capture(bus, microphone, 45000, 250);
  memset(g_storage, 0, sizeof(g_storage));
  capture.begin(g_storage, kCapacity, g_captureChunk, 512);

  tth::TurnBuffer& buffer = capture.turn();
  tth::LocalMockTurnSource mock(buffer, config());
  tth::TurnStreamer streamer(kFrame, 100);
  mock.poll(kStart);

  recordInto(buffer, kTurn);
  TEST_ASSERT_TRUE(streamer.begin(buffer, mock, tth::monoS16(16000)));
  TEST_ASSERT_TRUE(streamer.service(true) == tth::StreamStatus::Finished);

  // Thinking.
  TEST_ASSERT_FALSE(capture.start(kStart + 100));
  TEST_ASSERT_EQUAL(tth::CaptureStopReason::BufferInUse, capture.stopReason());
  TEST_ASSERT_EQUAL(tth::AudioOwner::None, bus.owner());
  TEST_ASSERT_EQUAL_UINT32(kTurn, buffer.committedSamples());

  // Responding.
  mock.poll(kStart + kThinkMs);
  tth::TurnEvent event;
  TEST_ASSERT_TRUE(mock.nextEvent(event));
  tth::AudioChunk chunk;
  TEST_ASSERT_TRUE(mock.peekPlaybackChunk(chunk, kSlot));
  mock.consumePlayback(chunk.count);
  TEST_ASSERT_FALSE(capture.start(kStart + kThinkMs + 10));
  TEST_ASSERT_EQUAL_UINT32(kTurn, buffer.committedSamples());

  while (mock.peekPlaybackChunk(chunk, kSlot)) mock.consumePlayback(chunk.count);

  // Everything consumed: now it may start, over a freshly reset buffer.
  TEST_ASSERT_TRUE(capture.start(kStart + 2000));
  TEST_ASSERT_EQUAL_UINT32(0, buffer.committedSamples());
}

// Abandoning the turn is the other way a dependency ends.
static void cancel_in_any_phase_releases_the_lease() {
  for (int phase = 0; phase < 3; ++phase) {
    Harness h;
    if (phase == 0) {
      // Receiving
      recordInto(h.buffer, kFrame);
      TEST_ASSERT_TRUE(h.mock.beginUserTurn(tth::monoS16(16000)));
      TEST_ASSERT_TRUE(h.mock.pushUserAudio(viewOf(h.buffer, 0, kFrame)) ==
                       tth::PushResult::Accepted);
    } else {
      h.recordAndStream();  // Thinking
      if (phase == 2) h.startResponse();  // Responding
    }
    TEST_ASSERT_TRUE(h.mock.holdsLease());
    TEST_ASSERT_FALSE(h.buffer.reset());

    h.mock.cancel();
    TEST_ASSERT_FALSE(h.mock.holdsLease());
    TEST_ASSERT_EQUAL_UINT32(0, h.buffer.readers());
    TEST_ASSERT_TRUE(h.buffer.reset());

    tth::TurnEvent event;
    h.mock.poll(kStart + 10 * kThinkMs);
    TEST_ASSERT_FALSE(h.mock.nextEvent(event));
    tth::AudioChunk chunk;
    TEST_ASSERT_FALSE(h.mock.peekPlaybackChunk(chunk, kSlot));
  }
}

// The mode is latched when the turn begins, so switching it mid-turn cannot
// drop the lease of a loopback turn that is still going to replay.
static void the_mode_is_latched_per_turn() {
  Harness h;
  recordInto(h.buffer, kFrame);
  TEST_ASSERT_TRUE(h.mock.beginUserTurn(tth::monoS16(16000)));
  h.mock.setMode(tth::MockMode::Synthetic);  // applies to the NEXT turn

  TEST_ASSERT_TRUE(h.mock.pushUserAudio(viewOf(h.buffer, 0, kFrame)) ==
                   tth::PushResult::Accepted);
  h.mock.endUserTurn();
  TEST_ASSERT_TRUE(h.mock.holdsLease());
  h.startResponse();
  TEST_ASSERT_TRUE(h.mock.turnMode() == tth::MockMode::Loopback);
  TEST_ASSERT_EQUAL_UINT32(16000, h.mock.responseFormat().sampleRate);
  TEST_ASSERT_TRUE(h.mock.holdsLease());
}

// Synthetic speech reads nothing from the recording, so once the streamer is
// done nothing holds the buffer.
static void synthetic_turns_hold_no_lease_on_the_recording() {
  Harness h;
  h.mock.setMode(tth::MockMode::Synthetic);
  h.recordAndStream();
  TEST_ASSERT_FALSE(h.mock.holdsLease());
  TEST_ASSERT_EQUAL_UINT32(0, h.buffer.readers());
  TEST_ASSERT_TRUE(h.buffer.reset());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(no_borrowed_chunk_is_relied_on_after_push_returns);
  RUN_TEST(only_the_next_committed_range_of_the_leased_buffer_is_accepted);
  RUN_TEST(the_buffer_is_protected_from_first_sample_to_last_copy);
  RUN_TEST(a_capture_cannot_start_while_loopback_still_depends_on_the_audio);
  RUN_TEST(cancel_in_any_phase_releases_the_lease);
  RUN_TEST(the_mode_is_latched_per_turn);
  RUN_TEST(synthetic_turns_hold_no_lease_on_the_recording);
  return UNITY_END();
}
