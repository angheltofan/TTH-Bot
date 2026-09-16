// Host-side tests for the immutable audio format contract.
//
// The central regression of Phase 5 is here: a 16 kHz chunk offered to a
// stream the speaker is playing at 24 kHz must be REJECTED -- not resampled,
// not coerced, not played. Nothing crashes if this goes wrong; it just sounds
// wrong, which is why it needs a test rather than a code review.

#include <string.h>

#include <deque>
#include <string>

#include <unity.h>

#include "tth/AudioBus.h"
#include "tth/AudioFormat.h"
#include "tth/PcmPlayer.h"

namespace {

const uint32_t kSlot = 16;
const uint32_t kDrainTimeoutMs = 400;
int16_t g_slots[3][kSlot];

class FakeAudioDevice : public tth::IAudioDevice {
 public:
  bool micBegin() override { return true; }
  void micEnd() override {}
  bool speakerBegin() override { return true; }
  void speakerEnd() override {}
};

struct Request {
  const int16_t* samples;
  uint32_t count;
  uint32_t rate;
};

class RecordingSpeaker : public tth::ISpeakerOutput {
 public:
  std::deque<Request> held;
  uint32_t plays = 0;

  uint32_t slotsOccupied() const override {
    return static_cast<uint32_t>(held.size());
  }
  bool play(const int16_t* samples, uint32_t count,
            uint32_t sampleRate) override {
    if (held.size() >= tth::kSpeakerQueueDepth) {
      TEST_FAIL_MESSAGE("play() with both speaker slots occupied");
    }
    Request request = {samples, count, sampleRate};
    held.push_back(request);
    ++plays;
    return true;
  }
  void stop() override {}
};

struct Harness {
  FakeAudioDevice device;
  tth::AudioBus bus;
  RecordingSpeaker speaker;
  tth::PcmPlayer player;

  Harness() : bus(device), player(bus, speaker, kDrainTimeoutMs) {
    memset(g_slots, 0, sizeof(g_slots));
    player.begin(g_slots[0], g_slots[1], g_slots[2], kSlot);
  }
};

int16_t g_pcm[kSlot];

tth::AudioChunk chunkAt(uint32_t rate) {
  for (uint32_t i = 0; i < kSlot; ++i) g_pcm[i] = static_cast<int16_t>(i * 100);
  tth::AudioChunk chunk;
  chunk.samples = g_pcm;
  chunk.count = kSlot;
  chunk.format = tth::monoS16(rate);
  return chunk;
}

}  // namespace

void setUp() {}
void tearDown() {}

static void mono_s16_is_mono_signed_16_bit_little_endian() {
  const tth::AudioFormat f = tth::monoS16(16000);
  TEST_ASSERT_EQUAL_UINT32(16000, f.sampleRate);
  TEST_ASSERT_EQUAL_UINT8(1, f.channels);
  TEST_ASSERT_TRUE(f.encoding == tth::SampleEncoding::S16LE);
}

static void formats_match_only_when_every_field_matches() {
  const tth::AudioFormat a = tth::monoS16(16000);
  tth::AudioFormat b = tth::monoS16(16000);
  TEST_ASSERT_TRUE(tth::sameFormat(a, b));

  b.sampleRate = 24000;
  TEST_ASSERT_FALSE(tth::sameFormat(a, b));

  b = tth::monoS16(16000);
  b.channels = 2;
  TEST_ASSERT_FALSE(tth::sameFormat(a, b));
}

static void only_mono_s16le_at_a_sane_rate_is_playable() {
  TEST_ASSERT_TRUE(tth::isPlayable(tth::monoS16(16000)));
  TEST_ASSERT_TRUE(tth::isPlayable(tth::monoS16(24000)));
  TEST_ASSERT_TRUE(tth::isPlayable(tth::monoS16(tth::kMinPlayableRate)));
  TEST_ASSERT_TRUE(tth::isPlayable(tth::monoS16(tth::kMaxPlayableRate)));

  TEST_ASSERT_FALSE(tth::isPlayable(tth::monoS16(0)));
  TEST_ASSERT_FALSE(tth::isPlayable(tth::monoS16(tth::kMinPlayableRate - 1)));
  TEST_ASSERT_FALSE(tth::isPlayable(tth::monoS16(96000)));

  tth::AudioFormat stereo = tth::monoS16(24000);
  stereo.channels = 2;
  TEST_ASSERT_FALSE(tth::isPlayable(stereo));
}

// THE central regression test of this phase.
static void a_16khz_chunk_is_rejected_by_a_24khz_stream() {
  Harness h;
  TEST_ASSERT_TRUE(h.player.openStream(tth::monoS16(24000), 0));

  uint32_t accepted = 99;
  const tth::PlayerPush result = h.player.submit(chunkAt(16000), accepted);

  TEST_ASSERT_TRUE(result == tth::PlayerPush::FormatMismatch);
  TEST_ASSERT_EQUAL_UINT32(0, accepted);
  TEST_ASSERT_EQUAL_UINT32(1, h.player.metrics().formatRejects);
  TEST_ASSERT_EQUAL_UINT32(0, h.player.metrics().queued);

  // And nothing reaches the speaker, however long it is serviced.
  for (uint32_t t = 0; t < 200; t += 10) h.player.service(t);
  TEST_ASSERT_EQUAL_UINT32(0, h.speaker.plays);
}

static void a_24khz_chunk_is_rejected_by_a_16khz_stream() {
  Harness h;
  TEST_ASSERT_TRUE(h.player.openStream(tth::monoS16(16000), 0));

  uint32_t accepted = 0;
  TEST_ASSERT_TRUE(h.player.submit(chunkAt(24000), accepted) ==
                   tth::PlayerPush::FormatMismatch);
  h.player.service(0);
  TEST_ASSERT_EQUAL_UINT32(0, h.speaker.plays);
}

// Native-rate playback: each stream reaches the speaker at its OWN rate,
// passed explicitly on every call. No resampler exists to get this wrong.
static void each_stream_plays_at_its_declared_rate() {
  {
    Harness h;
    TEST_ASSERT_TRUE(h.player.openStream(tth::monoS16(16000), 0));
    uint32_t accepted = 0;
    TEST_ASSERT_TRUE(h.player.submit(chunkAt(16000), accepted) ==
                     tth::PlayerPush::Accepted);
    h.player.service(0);
    TEST_ASSERT_EQUAL_UINT32(1, h.speaker.plays);
    TEST_ASSERT_EQUAL_UINT32(16000, h.speaker.held.front().rate);
  }
  {
    Harness h;
    TEST_ASSERT_TRUE(h.player.openStream(tth::monoS16(24000), 0));
    uint32_t accepted = 0;
    TEST_ASSERT_TRUE(h.player.submit(chunkAt(24000), accepted) ==
                     tth::PlayerPush::Accepted);
    h.player.service(0);
    TEST_ASSERT_EQUAL_UINT32(24000, h.speaker.held.front().rate);
  }
}

static void an_unplayable_stream_is_refused_at_open() {
  Harness h;
  tth::AudioFormat stereo = tth::monoS16(24000);
  stereo.channels = 2;

  TEST_ASSERT_FALSE(h.player.openStream(stereo, 0));
  TEST_ASSERT_FALSE(h.player.openStream(tth::monoS16(0), 0));
  TEST_ASSERT_TRUE(h.player.isIdle());
  // Refused before the bus was touched.
  TEST_ASSERT_EQUAL(tth::AudioOwner::None, h.bus.owner());
}

static void a_stereo_chunk_is_rejected_even_at_the_right_rate() {
  Harness h;
  TEST_ASSERT_TRUE(h.player.openStream(tth::monoS16(24000), 0));
  tth::AudioChunk chunk = chunkAt(24000);
  chunk.format.channels = 2;

  uint32_t accepted = 0;
  TEST_ASSERT_TRUE(h.player.submit(chunk, accepted) ==
                   tth::PlayerPush::FormatMismatch);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(mono_s16_is_mono_signed_16_bit_little_endian);
  RUN_TEST(formats_match_only_when_every_field_matches);
  RUN_TEST(only_mono_s16le_at_a_sane_rate_is_playable);
  RUN_TEST(a_16khz_chunk_is_rejected_by_a_24khz_stream);
  RUN_TEST(a_24khz_chunk_is_rejected_by_a_16khz_stream);
  RUN_TEST(each_stream_plays_at_its_declared_rate);
  RUN_TEST(an_unplayable_stream_is_refused_at_open);
  RUN_TEST(a_stereo_chunk_is_rejected_even_at_the_right_rate);
  return UNITY_END();
}
