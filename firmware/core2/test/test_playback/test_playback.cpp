// Host-side tests for PcmPlayer: non-blocking playback through a fixed ring
// of three preallocated slots.
//
// The speaker fake is STRICT: asking it to queue a third request fails the
// test outright, because on the device that call would spin inside
// M5Unified's playRaw() for a whole chunk.

#include <string.h>

#include <deque>
#include <string>
#include <vector>

#include <unity.h>

#include "tth/AudioBus.h"
#include "tth/AudioFormat.h"
#include "tth/PcmPlayer.h"

namespace {

const uint32_t kSlot = 8;
const uint32_t kDrainTimeoutMs = 400;
int16_t g_slots[3][kSlot];

class FakeAudioDevice : public tth::IAudioDevice {
 public:
  std::string log;
  bool speakerBeginResult = true;

  bool micBegin() override {
    log += "bus.mic+ ";
    return true;
  }
  void micEnd() override { log += "bus.mic- "; }
  bool speakerBegin() override {
    log += speakerBeginResult ? "bus.spk+ " : "bus.spkx ";
    return speakerBeginResult;
  }
  void speakerEnd() override { log += "bus.spk- "; }
};

struct Request {
  const int16_t* samples;
  uint32_t count;
  uint32_t rate;
};

class StrictSpeaker : public tth::ISpeakerOutput {
 public:
  std::deque<Request> held;
  std::vector<Request> history;
  bool refuse = false;
  uint32_t stops = 0;

  uint32_t slotsOccupied() const override {
    return static_cast<uint32_t>(held.size());
  }
  bool play(const int16_t* samples, uint32_t count,
            uint32_t sampleRate) override {
    if (held.size() >= tth::kSpeakerQueueDepth) {
      TEST_FAIL_MESSAGE(
          "play() with both speaker slots occupied: would block in playRaw");
    }
    if (refuse) return false;
    Request request = {samples, count, sampleRate};
    held.push_back(request);
    history.push_back(request);
    return true;
  }
  void stop() override { ++stops; }

  void finishOne() {
    if (!held.empty()) held.pop_front();
  }
  void finishAll() { held.clear(); }
};

struct Harness {
  FakeAudioDevice device;
  tth::AudioBus bus;
  StrictSpeaker speaker;
  tth::PcmPlayer player;

  Harness() : bus(device), player(bus, speaker, kDrainTimeoutMs) {
    memset(g_slots, 0, sizeof(g_slots));
    player.begin(g_slots[0], g_slots[1], g_slots[2], kSlot);
  }

  bool open(uint32_t rate = 24000) {
    return player.openStream(tth::monoS16(rate), 0);
  }
};

// Independent storage per chunk, so the source data is never the slot data.
struct Pcm {
  int16_t samples[kSlot * 2];
  tth::AudioChunk chunk(uint32_t count = kSlot, uint32_t rate = 24000) {
    tth::AudioChunk c;
    c.samples = samples;
    c.count = count;
    c.format = tth::monoS16(rate);
    return c;
  }
};

Pcm constant(int16_t value) {
  Pcm pcm;
  for (uint32_t i = 0; i < kSlot * 2; ++i) pcm.samples[i] = value;
  return pcm;
}

// Loud enough to have a clearly non-zero RMS.
Pcm loud() {
  Pcm pcm;
  for (uint32_t i = 0; i < kSlot * 2; ++i) {
    pcm.samples[i] = (i & 1u) ? static_cast<int16_t>(-20000) : 20000;
  }
  return pcm;
}

tth::PlayerPush submit(tth::PcmPlayer& player, Pcm& pcm,
                       uint32_t count = kSlot) {
  uint32_t accepted = 0;
  return player.submit(pcm.chunk(count), accepted);
}

bool isOneOfTheSlots(const int16_t* pointer) {
  return pointer == g_slots[0] || pointer == g_slots[1] ||
         pointer == g_slots[2];
}

}  // namespace

void setUp() {}
void tearDown() {}

// --- bus ownership ----------------------------------------------------------

static void opening_a_stream_acquires_the_speaker_through_the_bus() {
  Harness h;
  TEST_ASSERT_TRUE(h.open());
  TEST_ASSERT_TRUE(h.bus.speakerOwns());
  TEST_ASSERT_EQUAL_STRING("bus.spk+ ", h.device.log.c_str());
}

// Half duplex: a capture still draining owns the microphone, and the speaker
// may not be started underneath it.
static void a_stream_cannot_open_while_the_microphone_owns_the_bus() {
  Harness h;
  TEST_ASSERT_TRUE(h.bus.acquireMic());
  h.device.log.clear();

  TEST_ASSERT_FALSE(h.open());
  TEST_ASSERT_TRUE(h.bus.micOwns());
  TEST_ASSERT_EQUAL_STRING("", h.device.log.c_str());
  TEST_ASSERT_TRUE(h.player.isIdle());
}

static void a_stream_cannot_open_without_slots_or_twice() {
  {
    FakeAudioDevice device;
    tth::AudioBus bus(device);
    StrictSpeaker speaker;
    tth::PcmPlayer player(bus, speaker, kDrainTimeoutMs);
    TEST_ASSERT_FALSE(player.openStream(tth::monoS16(24000), 0));
    TEST_ASSERT_EQUAL(tth::AudioOwner::None, bus.owner());
  }
  {
    Harness h;
    TEST_ASSERT_TRUE(h.open());
    TEST_ASSERT_FALSE(h.open());
  }
}

static void a_failed_speaker_start_leaves_nothing_owned() {
  Harness h;
  h.device.speakerBeginResult = false;
  TEST_ASSERT_FALSE(h.open());
  TEST_ASSERT_EQUAL(tth::AudioOwner::None, h.bus.owner());
  TEST_ASSERT_TRUE(h.player.isIdle());
}

// --- the non-blocking invariant ---------------------------------------------

// The strict fake fails the test if a third request is ever queued, however
// many times the player is serviced.
static void never_queues_a_third_request_to_the_speaker() {
  Harness h;
  TEST_ASSERT_TRUE(h.open());
  Pcm a = constant(1), b = constant(2), c = constant(3);
  submit(h.player, a);
  submit(h.player, b);
  submit(h.player, c);

  for (uint32_t t = 0; t < 1000; t += 5) h.player.service(t);

  TEST_ASSERT_EQUAL_UINT32(2, h.speaker.slotsOccupied());
  TEST_ASSERT_EQUAL_UINT32(1, h.player.filledSlots());
  TEST_ASSERT_EQUAL_UINT32(2, h.player.playingSlots());
}

static void a_full_ring_returns_busy_and_takes_nothing() {
  Harness h;
  TEST_ASSERT_TRUE(h.open());
  Pcm a = constant(1), b = constant(2), c = constant(3), d = constant(4);
  TEST_ASSERT_TRUE(submit(h.player, a) == tth::PlayerPush::Accepted);
  TEST_ASSERT_TRUE(submit(h.player, b) == tth::PlayerPush::Accepted);
  TEST_ASSERT_TRUE(submit(h.player, c) == tth::PlayerPush::Accepted);
  TEST_ASSERT_FALSE(h.player.canAccept());

  uint32_t accepted = 99;
  TEST_ASSERT_TRUE(h.player.submit(d.chunk(), accepted) ==
                   tth::PlayerPush::Busy);
  TEST_ASSERT_EQUAL_UINT32(0, accepted);
  TEST_ASSERT_EQUAL_UINT32(3, h.player.metrics().queued);
}

// The Phase 4 lesson, on the output side: a slot the speaker task is still
// reading must not be overwritten.
static void a_slot_the_speaker_holds_is_never_reused() {
  Harness h;
  TEST_ASSERT_TRUE(h.open());
  Pcm a = constant(100), b = constant(200), c = constant(300),
      d = constant(400);
  submit(h.player, a);
  submit(h.player, b);
  submit(h.player, c);
  h.player.service(0);  // A and B go to the speaker; C waits.

  // D has nowhere to go: A and B are held, C is filled.
  TEST_ASSERT_TRUE(submit(h.player, d) == tth::PlayerPush::Busy);

  h.speaker.finishOne();  // A done
  h.player.service(1);    // A retired, C queued
  TEST_ASSERT_TRUE(submit(h.player, d) == tth::PlayerPush::Accepted);

  // What the speaker holds now (B then C) is still intact.
  TEST_ASSERT_EQUAL_UINT32(2, h.speaker.slotsOccupied());
  for (uint32_t i = 0; i < kSlot; ++i) {
    TEST_ASSERT_EQUAL_INT16(200, h.speaker.held[0].samples[i]);
    TEST_ASSERT_EQUAL_INT16(300, h.speaker.held[1].samples[i]);
  }
}

// No allocation: everything the speaker is ever given is one of the three
// preallocated slots.
static void plays_only_from_the_preallocated_slots() {
  Harness h;
  TEST_ASSERT_TRUE(h.open());
  for (int i = 0; i < 20; ++i) {
    Pcm pcm = constant(static_cast<int16_t>(i));
    while (submit(h.player, pcm) != tth::PlayerPush::Accepted) {
      h.speaker.finishOne();
      h.player.service(static_cast<uint32_t>(i));
    }
    h.player.service(static_cast<uint32_t>(i));
  }
  TEST_ASSERT_TRUE(h.speaker.history.size() > 3);
  for (size_t i = 0; i < h.speaker.history.size(); ++i) {
    TEST_ASSERT_TRUE(isOneOfTheSlots(h.speaker.history[i].samples));
  }
}

static void a_chunk_larger_than_a_slot_is_taken_one_slot_at_a_time() {
  Harness h;
  TEST_ASSERT_TRUE(h.open());
  Pcm big = constant(7);
  uint32_t accepted = 0;
  TEST_ASSERT_TRUE(h.player.submit(big.chunk(kSlot * 2), accepted) ==
                   tth::PlayerPush::Accepted);
  TEST_ASSERT_EQUAL_UINT32(kSlot, accepted);
}

static void a_refused_play_is_counted_and_retried() {
  Harness h;
  TEST_ASSERT_TRUE(h.open());
  Pcm a = constant(1);
  submit(h.player, a);

  h.speaker.refuse = true;
  h.player.service(0);
  TEST_ASSERT_EQUAL_UINT32(1, h.player.metrics().playRefusals);
  TEST_ASSERT_EQUAL_UINT32(1, h.player.filledSlots());

  h.speaker.refuse = false;
  h.player.service(1);
  TEST_ASSERT_EQUAL_UINT32(0, h.player.filledSlots());
  TEST_ASSERT_EQUAL_UINT32(1, h.speaker.slotsOccupied());
}

// --- counters and amplitude ---------------------------------------------------

static void queued_and_played_are_counted() {
  Harness h;
  TEST_ASSERT_TRUE(h.open());
  Pcm a = constant(1), b = constant(2);
  submit(h.player, a);
  submit(h.player, b);
  h.player.service(0);
  TEST_ASSERT_EQUAL_UINT32(2, h.player.metrics().queued);
  TEST_ASSERT_EQUAL_UINT32(0, h.player.metrics().played);
  TEST_ASSERT_EQUAL_UINT32(2 * kSlot, h.player.metrics().samplesQueued);

  h.speaker.finishAll();
  h.player.service(1);
  TEST_ASSERT_EQUAL_UINT32(2, h.player.metrics().played);
}

// Underrun: the speaker ran dry mid-stream. Once per episode, and never at the
// natural end of a stream.
static void an_underrun_is_counted_once_per_episode_mid_stream_only() {
  Harness h;
  TEST_ASSERT_TRUE(h.open());
  h.player.service(0);
  TEST_ASSERT_EQUAL_UINT32(0, h.player.metrics().underruns);  // not started

  Pcm a = constant(1);
  submit(h.player, a);
  h.player.service(1);
  h.speaker.finishAll();
  h.player.service(2);
  h.player.service(3);
  h.player.service(4);
  TEST_ASSERT_EQUAL_UINT32(1, h.player.metrics().underruns);

  Pcm b = constant(2);
  submit(h.player, b);
  h.player.service(5);  // recovered
  h.speaker.finishAll();
  h.player.markEndOfStream(6);
  h.player.service(7);  // dry, but at the end: not an underrun
  TEST_ASSERT_EQUAL_UINT32(1, h.player.metrics().underruns);
}

// The bars follow the chunk being HEARD -- the oldest the speaker holds --
// not the newest one queued.
static void the_level_is_that_of_the_chunk_being_played() {
  Harness h;
  TEST_ASSERT_TRUE(h.open());
  Pcm noisy = loud(), silent = constant(0);
  submit(h.player, noisy);
  submit(h.player, silent);
  h.player.service(0);

  TEST_ASSERT_TRUE(h.player.currentLevel() > 0.5f);

  h.speaker.finishOne();
  h.player.service(1);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, h.player.currentLevel());

  h.speaker.finishOne();
  h.player.service(2);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, h.player.currentLevel());
  TEST_ASSERT_TRUE(h.player.metrics().maxLevel > 0.5f);
}

static void nothing_is_reported_before_the_speaker_holds_anything() {
  Harness h;
  TEST_ASSERT_TRUE(h.open());
  Pcm noisy = loud();
  submit(h.player, noisy);
  // Queued in the ring, not yet handed to the speaker.
  TEST_ASSERT_EQUAL_FLOAT(0.0f, h.player.currentLevel());
}

// --- haptics latch -----------------------------------------------------------

static void first_audio_fires_once_when_the_speaker_accepts() {
  Harness h;
  TEST_ASSERT_TRUE(h.open());
  TEST_ASSERT_FALSE(h.player.consumeFirstAudio());

  Pcm a = constant(1);
  submit(h.player, a);
  // Accepted into the ring is not enough: the speaker has not taken it.
  TEST_ASSERT_FALSE(h.player.consumeFirstAudio());

  h.player.service(0);
  TEST_ASSERT_TRUE(h.player.consumeFirstAudio());
  TEST_ASSERT_FALSE(h.player.consumeFirstAudio());

  for (int i = 0; i < 20; ++i) {
    Pcm pcm = constant(2);
    submit(h.player, pcm);
    h.speaker.finishAll();
    h.player.service(static_cast<uint32_t>(i + 1));
    TEST_ASSERT_FALSE(h.player.consumeFirstAudio());
  }
}

static void first_audio_never_fires_if_the_speaker_never_accepts() {
  Harness h;
  TEST_ASSERT_TRUE(h.open());
  h.speaker.refuse = true;
  Pcm a = constant(1);
  submit(h.player, a);
  for (uint32_t t = 0; t < 100; ++t) h.player.service(t);
  TEST_ASSERT_FALSE(h.player.consumeFirstAudio());
}

// --- ending, cancelling, releasing --------------------------------------------

static void a_completed_stream_drains_then_releases_the_speaker() {
  Harness h;
  TEST_ASSERT_TRUE(h.open());
  Pcm a = constant(1), b = constant(2);
  submit(h.player, a);
  submit(h.player, b);
  h.player.markEndOfStream(0);
  h.player.service(0);
  TEST_ASSERT_TRUE(h.player.state() == tth::PlayerState::Ending);

  h.speaker.finishAll();
  h.player.service(50);
  TEST_ASSERT_TRUE(h.player.isDrained());
  TEST_ASSERT_TRUE(h.player.endReason() == tth::PlaybackEnd::Completed);
  // Drained is not released: the speaker is still owned until release().
  TEST_ASSERT_TRUE(h.bus.speakerOwns());

  TEST_ASSERT_TRUE(h.player.release());
  TEST_ASSERT_TRUE(h.player.isIdle());
  TEST_ASSERT_EQUAL(tth::AudioOwner::None, h.bus.owner());
  TEST_ASSERT_EQUAL_STRING("bus.spk+ bus.spk- ", h.device.log.c_str());
}

static void cancel_stops_the_speaker_and_waits_for_it_to_let_go() {
  Harness h;
  TEST_ASSERT_TRUE(h.open());
  Pcm a = constant(1), b = constant(2), c = constant(3);
  submit(h.player, a);
  submit(h.player, b);
  submit(h.player, c);
  h.player.service(0);  // two held, one filled

  h.player.cancel(10, tth::PlaybackEnd::Cancelled);
  TEST_ASSERT_EQUAL_UINT32(1, h.speaker.stops);
  TEST_ASSERT_TRUE(h.player.state() == tth::PlayerState::Stopping);
  TEST_ASSERT_EQUAL_UINT32(0, h.player.filledSlots());

  // No new audio is taken, and the held slots are not reused, while the
  // speaker still holds them.
  Pcm d = constant(4);
  TEST_ASSERT_TRUE(submit(h.player, d) == tth::PlayerPush::NotOpen);
  h.player.service(20);
  TEST_ASSERT_TRUE(h.player.state() == tth::PlayerState::Stopping);
  TEST_ASSERT_EQUAL_UINT32(2, h.speaker.slotsOccupied());

  h.speaker.finishAll();
  h.player.service(30);
  TEST_ASSERT_TRUE(h.player.isDrained());
  TEST_ASSERT_TRUE(h.player.endReason() == tth::PlaybackEnd::Cancelled);
  TEST_ASSERT_EQUAL_UINT32(20, h.player.lastDrainMs());

  TEST_ASSERT_TRUE(h.player.release());
  TEST_ASSERT_EQUAL(tth::AudioOwner::None, h.bus.owner());
}

static void an_error_also_ends_in_a_released_speaker() {
  Harness h;
  TEST_ASSERT_TRUE(h.open());
  Pcm a = constant(1);
  submit(h.player, a);
  h.player.service(0);

  h.player.cancel(0, tth::PlaybackEnd::Error);
  h.speaker.finishAll();
  h.player.service(1);
  TEST_ASSERT_TRUE(h.player.endReason() == tth::PlaybackEnd::Error);
  TEST_ASSERT_TRUE(h.player.release());
  TEST_ASSERT_EQUAL(tth::AudioOwner::None, h.bus.owner());
}

static void a_speaker_that_never_goes_quiet_is_given_up_on_at_the_deadline() {
  Harness h;
  TEST_ASSERT_TRUE(h.open());
  Pcm a = constant(1);
  submit(h.player, a);
  h.player.service(0);
  h.player.cancel(100, tth::PlaybackEnd::Cancelled);

  h.player.service(100 + kDrainTimeoutMs - 1);
  TEST_ASSERT_TRUE(h.player.state() == tth::PlayerState::Stopping);

  h.player.service(100 + kDrainTimeoutMs);
  TEST_ASSERT_TRUE(h.player.isDrained());
  TEST_ASSERT_EQUAL_UINT32(1, h.player.metrics().drainTimeouts);
}

static void release_is_refused_unless_drained() {
  Harness h;
  TEST_ASSERT_FALSE(h.player.release());  // idle
  TEST_ASSERT_TRUE(h.open());
  TEST_ASSERT_FALSE(h.player.release());  // streaming
  TEST_ASSERT_TRUE(h.bus.speakerOwns());
}

static void counters_start_fresh_for_every_stream() {
  Harness h;
  TEST_ASSERT_TRUE(h.open());
  Pcm a = constant(1);
  submit(h.player, a);
  h.player.markEndOfStream(0);
  h.player.service(0);
  h.speaker.finishAll();
  h.player.service(1);
  h.player.release();
  TEST_ASSERT_EQUAL_UINT32(1, h.player.metrics().queued);

  TEST_ASSERT_TRUE(h.open(16000));
  TEST_ASSERT_EQUAL_UINT32(0, h.player.metrics().queued);
  TEST_ASSERT_EQUAL_UINT32(0, h.player.metrics().played);
  TEST_ASSERT_FALSE(h.player.consumeFirstAudio());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(opening_a_stream_acquires_the_speaker_through_the_bus);
  RUN_TEST(a_stream_cannot_open_while_the_microphone_owns_the_bus);
  RUN_TEST(a_stream_cannot_open_without_slots_or_twice);
  RUN_TEST(a_failed_speaker_start_leaves_nothing_owned);
  RUN_TEST(never_queues_a_third_request_to_the_speaker);
  RUN_TEST(a_full_ring_returns_busy_and_takes_nothing);
  RUN_TEST(a_slot_the_speaker_holds_is_never_reused);
  RUN_TEST(plays_only_from_the_preallocated_slots);
  RUN_TEST(a_chunk_larger_than_a_slot_is_taken_one_slot_at_a_time);
  RUN_TEST(a_refused_play_is_counted_and_retried);
  RUN_TEST(queued_and_played_are_counted);
  RUN_TEST(an_underrun_is_counted_once_per_episode_mid_stream_only);
  RUN_TEST(the_level_is_that_of_the_chunk_being_played);
  RUN_TEST(nothing_is_reported_before_the_speaker_holds_anything);
  RUN_TEST(first_audio_fires_once_when_the_speaker_accepts);
  RUN_TEST(first_audio_never_fires_if_the_speaker_never_accepts);
  RUN_TEST(a_completed_stream_drains_then_releases_the_speaker);
  RUN_TEST(cancel_stops_the_speaker_and_waits_for_it_to_let_go);
  RUN_TEST(an_error_also_ends_in_a_released_speaker);
  RUN_TEST(a_speaker_that_never_goes_quiet_is_given_up_on_at_the_deadline);
  RUN_TEST(release_is_refused_unless_drained);
  RUN_TEST(counters_start_fresh_for_every_stream);
  return UNITY_END();
}
