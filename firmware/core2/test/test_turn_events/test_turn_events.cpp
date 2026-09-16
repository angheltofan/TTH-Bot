// Host-side tests for the turn-source side of Phase 5: the bounded control
// event queue, and LocalMockTurnSource in both modes.
//
// Also the ownership rule: a borrowed chunk is not retained past the call --
// the consumer copies, so poisoning the source's memory afterwards cannot
// change what is played.

#include <string.h>

#include <deque>
#include <vector>

#include <unity.h>

#include "tth/AudioBus.h"
#include "tth/AudioFormat.h"
#include "tth/ITurnSource.h"
#include "tth/LocalMockTurnSource.h"
#include "tth/PcmPlayer.h"
#include "tth/TurnBuffer.h"
#include "tth/TurnEventQueue.h"

namespace {

const uint32_t kCapacity = 4000;
const uint32_t kFrame = 320;
const uint32_t kThinkMs = 600;
const uint32_t kSynthMs = 1000;  // 24000 samples at 24 kHz
const uint32_t kScratch = 960;
// Half a second in: past the envelope's ease-in, inside the first (quiet)
// phrase. The very start of a response is deliberately silent, so comparing
// it would prove nothing.
const uint32_t kMidResponse = 12000;

int16_t g_storage[kCapacity];
int16_t g_scratch[kScratch];

tth::TurnEvent eventOf(tth::TurnEventType type, uint32_t generation) {
  tth::TurnEvent event;
  event.type = type;
  event.error = tth::TurnError::None;
  event.format = tth::monoS16(16000);
  event.generation = generation;
  return event;
}

tth::MockConfig config() {
  tth::MockConfig c;
  c.captureFormat = tth::monoS16(16000);
  c.assistantFormat = tth::monoS16(24000);
  c.thinkMs = kThinkMs;
  c.synthDurationMs = kSynthMs;
  c.busyWindowMs = 150;
  c.busyPeriodMs = 400;
  return c;
}

tth::AudioChunk viewOf(const tth::TurnBuffer& buffer, uint32_t offset,
                       uint32_t count, uint32_t rate = 16000) {
  tth::AudioChunk chunk;
  chunk.samples = buffer.data() + offset;
  chunk.count = count;
  chunk.format = tth::monoS16(rate);
  return chunk;
}

// Consumes a responding mock up to `position`, the way a player would.
void advanceTo(tth::LocalMockTurnSource& mock, uint32_t position) {
  tth::AudioChunk chunk;
  while (mock.responseCursor() < position) {
    const uint32_t left = position - mock.responseCursor();
    if (!mock.peekPlaybackChunk(chunk, left < kScratch ? left : kScratch)) {
      TEST_FAIL_MESSAGE("response ended before the requested position");
    }
    mock.consumePlayback(chunk.count);
  }
}

int16_t maxAbs(const int16_t* samples, uint32_t count) {
  int32_t peak = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const int32_t v = samples[i] < 0 ? -static_cast<int32_t>(samples[i])
                                     : static_cast<int32_t>(samples[i]);
    if (v > peak) peak = v;
  }
  return static_cast<int16_t>(peak > 32767 ? 32767 : peak);
}

struct Harness {
  tth::TurnBuffer buffer;
  tth::LocalMockTurnSource mock;

  Harness() : mock(buffer, config()) {
    memset(g_storage, 0, sizeof(g_storage));
    memset(g_scratch, 0, sizeof(g_scratch));
    buffer.attach(g_storage, kCapacity);
    mock.attachSynthScratch(g_scratch, kScratch);
  }

  // Records `samples` of a ramp and hands every frame to the mock, exactly
  // the way TurnStreamer would.
  void recordTurn(uint32_t samples, uint32_t nowMs = 1000) {
    std::vector<int16_t> ramp(samples);
    for (uint32_t i = 0; i < samples; ++i) {
      ramp[i] = static_cast<int16_t>(i + 1);
    }
    buffer.append(ramp.data(), samples);
    mock.poll(nowMs);
    TEST_ASSERT_TRUE(mock.beginUserTurn(tth::monoS16(16000)));
    for (uint32_t offset = 0; offset < samples; offset += kFrame) {
      const uint32_t count =
          (samples - offset < kFrame) ? (samples - offset) : kFrame;
      TEST_ASSERT_TRUE(mock.pushUserAudio(viewOf(buffer, offset, count)) ==
                       tth::PushResult::Accepted);
    }
    mock.endUserTurn();
  }

  bool next(tth::TurnEvent& event) { return mock.nextEvent(event); }
};

// For the ownership test: a player behind a speaker that just holds requests.
class FakeAudioDevice : public tth::IAudioDevice {
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

int16_t g_slots[3][kScratch];

}  // namespace

void setUp() {}
void tearDown() {}

// --- the event queue ---------------------------------------------------------

static void events_come_out_in_the_order_they_went_in() {
  tth::TurnEventQueue queue;
  queue.push(eventOf(tth::TurnEventType::SpeechStart, 1));
  queue.push(eventOf(tth::TurnEventType::TurnComplete, 1));

  tth::TurnEvent out;
  TEST_ASSERT_TRUE(queue.pop(out, 1));
  TEST_ASSERT_TRUE(out.type == tth::TurnEventType::SpeechStart);
  TEST_ASSERT_TRUE(queue.pop(out, 1));
  TEST_ASSERT_TRUE(out.type == tth::TurnEventType::TurnComplete);
  TEST_ASSERT_FALSE(queue.pop(out, 1));
}

// Full: the NEWEST is refused and counted; nothing already queued is lost or
// reordered.
static void a_full_queue_refuses_the_newest_and_counts_it() {
  tth::TurnEventQueue queue;
  for (uint32_t i = 0; i < tth::TurnEventQueue::kSlots; ++i) {
    tth::TurnEvent event = eventOf(tth::TurnEventType::Error, 1);
    event.format.sampleRate = i;  // a marker
    TEST_ASSERT_TRUE(queue.push(event));
  }
  TEST_ASSERT_FALSE(queue.push(eventOf(tth::TurnEventType::SpeechStart, 1)));
  TEST_ASSERT_EQUAL_UINT32(1, queue.drops());
  TEST_ASSERT_EQUAL_UINT32(tth::TurnEventQueue::kSlots, queue.size());

  tth::TurnEvent out;
  for (uint32_t i = 0; i < tth::TurnEventQueue::kSlots; ++i) {
    TEST_ASSERT_TRUE(queue.pop(out, 1));
    TEST_ASSERT_EQUAL_UINT32(i, out.format.sampleRate);
  }
}

static void events_from_an_older_generation_are_discarded() {
  tth::TurnEventQueue queue;
  queue.push(eventOf(tth::TurnEventType::SpeechStart, 1));
  queue.push(eventOf(tth::TurnEventType::TurnComplete, 1));
  queue.push(eventOf(tth::TurnEventType::SpeechStart, 2));

  tth::TurnEvent out;
  TEST_ASSERT_TRUE(queue.pop(out, 2));
  TEST_ASSERT_TRUE(out.type == tth::TurnEventType::SpeechStart);
  TEST_ASSERT_EQUAL_UINT32(2, out.generation);
  TEST_ASSERT_EQUAL_UINT32(2, queue.staleDiscards());
}

// --- mock: loopback ---------------------------------------------------------

static void loopback_replays_the_recording_at_its_native_16khz() {
  Harness h;
  h.recordTurn(1000);

  tth::TurnEvent event;
  h.mock.poll(1000 + kThinkMs - 1);
  TEST_ASSERT_FALSE(h.next(event));  // still thinking: WAITING is visible

  h.mock.poll(1000 + kThinkMs);
  TEST_ASSERT_TRUE(h.next(event));
  TEST_ASSERT_TRUE(event.type == tth::TurnEventType::SpeechStart);
  TEST_ASSERT_EQUAL_UINT32(16000, event.format.sampleRate);

  // Straight out of the turn buffer, under a lease: no copy, no resample.
  TEST_ASSERT_TRUE(h.mock.holdsLease());
  TEST_ASSERT_FALSE(h.buffer.reset());

  uint32_t played = 0;
  tth::AudioChunk chunk;
  while (h.mock.peekPlaybackChunk(chunk, 300)) {
    TEST_ASSERT_EQUAL_UINT32(16000, chunk.format.sampleRate);
    TEST_ASSERT_EQUAL_PTR(h.buffer.data() + played, chunk.samples);
    played += chunk.count;
    h.mock.consumePlayback(chunk.count);
  }
  TEST_ASSERT_EQUAL_UINT32(1000, played);

  TEST_ASSERT_TRUE(h.next(event));
  TEST_ASSERT_TRUE(event.type == tth::TurnEventType::TurnComplete);
  TEST_ASSERT_FALSE(h.mock.holdsLease());
  TEST_ASSERT_TRUE(h.buffer.reset());
}

// Peeking is not consuming: a full player ring re-peeks the same samples.
static void peeking_twice_returns_the_same_samples() {
  Harness h;
  h.recordTurn(1000);
  h.mock.poll(1000 + kThinkMs);

  tth::AudioChunk first;
  tth::AudioChunk second;
  TEST_ASSERT_TRUE(h.mock.peekPlaybackChunk(first, 300));
  TEST_ASSERT_TRUE(h.mock.peekPlaybackChunk(second, 300));
  TEST_ASSERT_EQUAL_PTR(first.samples, second.samples);
  TEST_ASSERT_EQUAL_UINT32(0, h.mock.responseCursor());
}

static void an_empty_recording_completes_with_no_speech() {
  Harness h;
  h.mock.poll(1000);
  TEST_ASSERT_TRUE(h.mock.beginUserTurn(tth::monoS16(16000)));
  h.mock.endUserTurn();
  h.mock.poll(1000 + kThinkMs);

  tth::TurnEvent event;
  TEST_ASSERT_TRUE(h.next(event));
  TEST_ASSERT_TRUE(event.type == tth::TurnEventType::TurnComplete);
  TEST_ASSERT_FALSE(h.mock.holdsLease());
}

// --- mock: synthetic ----------------------------------------------------------

static void synthetic_speech_is_24khz_and_deterministic() {
  Harness h;
  h.mock.setMode(tth::MockMode::Synthetic);
  h.recordTurn(500);
  h.mock.poll(1000 + kThinkMs);

  tth::TurnEvent event;
  TEST_ASSERT_TRUE(h.next(event));
  TEST_ASSERT_TRUE(event.type == tth::TurnEventType::SpeechStart);
  TEST_ASSERT_EQUAL_UINT32(24000, event.format.sampleRate);
  TEST_ASSERT_EQUAL_UINT32(24000 * kSynthMs / 1000, h.mock.responseSamples());
  // Synthetic reads nothing from the recording.
  TEST_ASSERT_FALSE(h.mock.holdsLease());

  // Compare real audio, from the middle of the response.
  advanceTo(h.mock, kMidResponse);
  tth::AudioChunk chunk;
  TEST_ASSERT_TRUE(h.mock.peekPlaybackChunk(chunk, kScratch));
  TEST_ASSERT_EQUAL_UINT32(24000, chunk.format.sampleRate);
  const std::vector<int16_t> first(chunk.samples, chunk.samples + chunk.count);
  TEST_ASSERT_TRUE(maxAbs(first.data(), chunk.count) > 500);

  // A second source, walked to the same position, produces the same samples.
  memset(g_scratch, 0, sizeof(g_scratch));
  tth::LocalMockTurnSource again(h.buffer, config());
  again.attachSynthScratch(g_scratch, kScratch);
  again.setMode(tth::MockMode::Synthetic);
  again.poll(1000);
  TEST_ASSERT_TRUE(again.beginUserTurn(tth::monoS16(16000)));
  again.endUserTurn();
  again.poll(1000 + kThinkMs);
  advanceTo(again, kMidResponse);
  tth::AudioChunk repeat;
  TEST_ASSERT_TRUE(again.peekPlaybackChunk(repeat, kScratch));
  TEST_ASSERT_EQUAL_UINT32(chunk.count, repeat.count);
  for (uint32_t i = 0; i < chunk.count; ++i) {
    TEST_ASSERT_EQUAL_INT16(first[i], repeat.samples[i]);
  }

  // And the whole response is exactly the declared length.
  while (h.mock.peekPlaybackChunk(chunk, kScratch)) {
    h.mock.consumePlayback(chunk.count);
  }
  TEST_ASSERT_EQUAL_UINT32(24000 * kSynthMs / 1000, h.mock.responseCursor());
  TEST_ASSERT_TRUE(h.next(event));
  TEST_ASSERT_TRUE(event.type == tth::TurnEventType::TurnComplete);
}

// --- ownership ----------------------------------------------------------------

// A borrowed chunk is not retained past the call. The player copies it; the
// source then poisons its own memory; what the speaker plays is unchanged.
static void a_borrowed_chunk_is_copied_before_the_source_reuses_it() {
  Harness h;
  h.mock.setMode(tth::MockMode::Synthetic);
  h.recordTurn(500);
  h.mock.poll(1000 + kThinkMs);
  tth::TurnEvent event;
  TEST_ASSERT_TRUE(h.next(event));
  advanceTo(h.mock, kMidResponse);

  FakeAudioDevice device;
  tth::AudioBus bus(device);
  HoldingSpeaker speaker;
  tth::PcmPlayer player(bus, speaker, 400);
  player.begin(g_slots[0], g_slots[1], g_slots[2], kScratch);
  TEST_ASSERT_TRUE(player.openStream(event.format, 0));

  tth::AudioChunk chunk;
  TEST_ASSERT_TRUE(h.mock.peekPlaybackChunk(chunk, kScratch));
  const std::vector<int16_t> original(chunk.samples,
                                      chunk.samples + chunk.count);
  // Real audio, so the poison below is distinguishable from it.
  TEST_ASSERT_TRUE(maxAbs(original.data(), chunk.count) > 500);

  uint32_t accepted = 0;
  TEST_ASSERT_TRUE(player.submit(chunk, accepted) ==
                   tth::PlayerPush::Accepted);
  h.mock.consumePlayback(accepted);

  // Poison the source's buffer after the call has returned.
  for (uint32_t i = 0; i < kScratch; ++i) {
    g_scratch[i] = static_cast<int16_t>(0x5A5A);
  }

  player.service(0);
  TEST_ASSERT_EQUAL_UINT32(1, speaker.held.size());
  for (uint32_t i = 0; i < accepted; ++i) {
    TEST_ASSERT_EQUAL_INT16(original[i], speaker.held.front()[i]);
  }
}

// --- cancel, errors, contract ---------------------------------------------------

// No late delivery: an event queued before cancel() is never delivered, and
// no more audio is handed out.
static void nothing_from_a_cancelled_turn_is_delivered() {
  Harness h;
  h.recordTurn(1000);
  h.mock.poll(1000 + kThinkMs);  // SpeechStart queued, not yet taken

  h.mock.cancel();

  tth::TurnEvent event;
  TEST_ASSERT_FALSE(h.next(event));
  TEST_ASSERT_EQUAL_UINT32(1, h.mock.staleDiscards());
  tth::AudioChunk chunk;
  TEST_ASSERT_FALSE(h.mock.peekPlaybackChunk(chunk, 300));
  TEST_ASSERT_FALSE(h.mock.holdsLease());
  TEST_ASSERT_TRUE(h.buffer.reset());
}

static void cancel_part_way_through_a_response_releases_the_lease() {
  Harness h;
  h.recordTurn(1000);
  h.mock.poll(1000 + kThinkMs);
  tth::TurnEvent event;
  TEST_ASSERT_TRUE(h.next(event));
  tth::AudioChunk chunk;
  TEST_ASSERT_TRUE(h.mock.peekPlaybackChunk(chunk, 300));
  h.mock.consumePlayback(chunk.count);

  h.mock.cancel();
  TEST_ASSERT_FALSE(h.mock.holdsLease());
  TEST_ASSERT_FALSE(h.buffer.isRetained());
  TEST_ASSERT_FALSE(h.next(event));  // no TurnComplete for an abandoned turn
}

static void an_injected_error_arrives_instead_of_speech() {
  Harness h;
  h.mock.injectErrorOnNextTurn();
  h.recordTurn(1000);
  h.mock.poll(1000 + kThinkMs);

  tth::TurnEvent event;
  TEST_ASSERT_TRUE(h.next(event));
  TEST_ASSERT_TRUE(event.type == tth::TurnEventType::Error);
  TEST_ASSERT_TRUE(event.error == tth::TurnError::Injected);
  TEST_ASSERT_FALSE(h.mock.holdsLease());
  TEST_ASSERT_FALSE(h.mock.errorPending());
}

static void pushes_outside_a_user_turn_or_in_the_wrong_format_are_fatal() {
  Harness h;
  tth::AudioChunk chunk = viewOf(h.buffer, 0, kFrame);
  TEST_ASSERT_TRUE(h.mock.pushUserAudio(chunk) == tth::PushResult::Fatal);

  TEST_ASSERT_TRUE(h.mock.beginUserTurn(tth::monoS16(16000)));
  tth::AudioChunk wrongRate = viewOf(h.buffer, 0, kFrame, 24000);
  TEST_ASSERT_TRUE(h.mock.pushUserAudio(wrongRate) == tth::PushResult::Fatal);

  tth::AudioChunk nullChunk = chunk;
  nullChunk.samples = nullptr;
  TEST_ASSERT_TRUE(h.mock.pushUserAudio(nullChunk) == tth::PushResult::Fatal);
}

static void a_user_turn_in_the_wrong_format_is_refused() {
  Harness h;
  TEST_ASSERT_FALSE(h.mock.beginUserTurn(tth::monoS16(24000)));
}

static void a_gap_in_the_pushed_audio_is_detected() {
  Harness h;
  int16_t ramp[2 * kFrame];
  for (uint32_t i = 0; i < 2 * kFrame; ++i) ramp[i] = static_cast<int16_t>(i);
  h.buffer.append(ramp, 2 * kFrame);
  TEST_ASSERT_TRUE(h.mock.beginUserTurn(tth::monoS16(16000)));

  // Skips the first frame. Detected AND refused: the mock would otherwise be
  // accepting audio it could not replay from its lease.
  TEST_ASSERT_TRUE(h.mock.pushUserAudio(viewOf(h.buffer, kFrame, kFrame)) ==
                   tth::PushResult::Fatal);
  TEST_ASSERT_EQUAL_UINT32(1, h.mock.continuityErrors());
  TEST_ASSERT_EQUAL_UINT32(0, h.mock.receivedSamples());
}

// The on-device backpressure diagnostic: Busy inside the window, Accepted
// outside it.
static void simulated_backpressure_returns_busy_in_its_window() {
  Harness h;
  int16_t ramp[kFrame] = {0};
  h.buffer.append(ramp, kFrame);
  h.mock.setBackpressure(true);
  TEST_ASSERT_TRUE(h.mock.beginUserTurn(tth::monoS16(16000)));

  h.mock.poll(1200);  // 1200 % 400 = 0: inside the 150 ms window
  TEST_ASSERT_TRUE(h.mock.pushUserAudio(viewOf(h.buffer, 0, kFrame)) ==
                   tth::PushResult::Busy);
  TEST_ASSERT_EQUAL_UINT32(0, h.mock.receivedSamples());

  h.mock.poll(1360);  // 160: outside
  TEST_ASSERT_TRUE(h.mock.pushUserAudio(viewOf(h.buffer, 0, kFrame)) ==
                   tth::PushResult::Accepted);
  TEST_ASSERT_EQUAL_UINT32(kFrame, h.mock.receivedSamples());
  TEST_ASSERT_EQUAL_UINT32(1, h.mock.busyReturned());
}

static void a_new_user_turn_is_refused_while_a_response_is_running() {
  Harness h;
  h.recordTurn(1000);
  h.mock.poll(1000 + kThinkMs);
  TEST_ASSERT_FALSE(h.mock.beginUserTurn(tth::monoS16(16000)));
  h.mock.cancel();
  TEST_ASSERT_TRUE(h.mock.beginUserTurn(tth::monoS16(16000)));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(events_come_out_in_the_order_they_went_in);
  RUN_TEST(a_full_queue_refuses_the_newest_and_counts_it);
  RUN_TEST(events_from_an_older_generation_are_discarded);
  RUN_TEST(loopback_replays_the_recording_at_its_native_16khz);
  RUN_TEST(peeking_twice_returns_the_same_samples);
  RUN_TEST(an_empty_recording_completes_with_no_speech);
  RUN_TEST(synthetic_speech_is_24khz_and_deterministic);
  RUN_TEST(a_borrowed_chunk_is_copied_before_the_source_reuses_it);
  RUN_TEST(nothing_from_a_cancelled_turn_is_delivered);
  RUN_TEST(cancel_part_way_through_a_response_releases_the_lease);
  RUN_TEST(an_injected_error_arrives_instead_of_speech);
  RUN_TEST(pushes_outside_a_user_turn_or_in_the_wrong_format_are_fatal);
  RUN_TEST(a_user_turn_in_the_wrong_format_is_refused);
  RUN_TEST(a_gap_in_the_pushed_audio_is_detected);
  RUN_TEST(simulated_backpressure_returns_busy_in_its_window);
  RUN_TEST(a_new_user_turn_is_refused_while_a_response_is_running);
  return UNITY_END();
}
