// Host-side tests for the loopback digital gain (tth/PcmGain.h) and its use
// inside LocalMockTurnSource's loopback path.
//
// Pinned: exact unity bypass; +9 dB = x2.82; saturation both ways; no integer
// wrap even at the maximum gain; silence stays silence; synthetic audio is
// untouched; the output does not depend on where chunk boundaries fall; one
// gain per turn (no pumping); and the soft limiter on loud input.

#include <string.h>

#include <deque>
#include <vector>

#include <unity.h>

#include "tth/AudioBus.h"
#include "tth/AudioFormat.h"
#include "tth/LocalMockTurnSource.h"
#include "tth/PcmGain.h"
#include "tth/PcmPlayer.h"
#include "tth/TurnBuffer.h"

namespace {

const uint32_t kCapacity = 8000;
const uint32_t kFrame = 320;
const uint32_t kScratch = 960;
const uint32_t kThinkMs = 600;

int16_t g_storage[kCapacity];
int16_t g_scratch[kScratch];

// The reference: sample * gain, rounded half away from zero, in 64 bits.
int32_t exactAmplify(int32_t sample, int32_t gainQ12) {
  const int64_t product = static_cast<int64_t>(sample) * gainQ12;
  return static_cast<int32_t>(product >= 0 ? (product + 2048) / 4096
                                           : -((-product + 2048) / 4096));
}

tth::MockConfig config(float gainDb) {
  tth::MockConfig c;
  c.captureFormat = tth::monoS16(16000);
  c.assistantFormat = tth::monoS16(24000);
  c.thinkMs = kThinkMs;
  c.synthDurationMs = 500;
  c.busyWindowMs = 0;
  c.busyPeriodMs = 0;
  c.loopbackGainDb = gainDb;
  return c;
}

// One complete mock turn over the shared storage.
struct Rig {
  tth::TurnBuffer buffer;
  tth::LocalMockTurnSource mock;

  explicit Rig(float gainDb, tth::MockMode mode = tth::MockMode::Loopback)
      : mock(buffer, config(gainDb)) {
    memset(g_storage, 0, sizeof(g_storage));
    memset(g_scratch, 0, sizeof(g_scratch));
    buffer.attach(g_storage, kCapacity);
    mock.attachSynthScratch(g_scratch, kScratch);
    mock.setMode(mode);
    mock.poll(1000);
  }

  // Records `pcm`, streams it in 20 ms frames, and starts the response.
  void speak(const std::vector<int16_t>& pcm) {
    if (!pcm.empty()) {
      buffer.append(pcm.data(), static_cast<uint32_t>(pcm.size()));
    }
    TEST_ASSERT_TRUE(mock.beginUserTurn(tth::monoS16(16000)));
    const uint32_t total = static_cast<uint32_t>(pcm.size());
    for (uint32_t offset = 0; offset < total; offset += kFrame) {
      tth::AudioChunk chunk;
      chunk.samples = buffer.data() + offset;
      chunk.count = (total - offset < kFrame) ? total - offset : kFrame;
      chunk.format = tth::monoS16(16000);
      TEST_ASSERT_TRUE(mock.pushUserAudio(chunk) == tth::PushResult::Accepted);
    }
    mock.endUserTurn();
    mock.poll(1000 + kThinkMs);
    tth::TurnEvent event;
    TEST_ASSERT_TRUE(mock.nextEvent(event));
    TEST_ASSERT_TRUE(event.type == tth::TurnEventType::SpeechStart);
  }

  // Everything the player would receive, peeking at most `maxSamples` a time.
  std::vector<int16_t> replay(uint32_t maxSamples) {
    std::vector<int16_t> out;
    tth::AudioChunk chunk;
    while (mock.peekPlaybackChunk(chunk, maxSamples)) {
      out.insert(out.end(), chunk.samples, chunk.samples + chunk.count);
      mock.consumePlayback(chunk.count);
    }
    return out;
  }
};

std::vector<int16_t> constant(uint32_t count, int16_t value) {
  return std::vector<int16_t>(count, value);
}

// Alternating +a/-a, so DC-free "speech" of a known peak.
std::vector<int16_t> square(uint32_t count, int16_t amplitude) {
  std::vector<int16_t> pcm(count);
  for (uint32_t i = 0; i < count; ++i) {
    pcm[i] = (i & 1u) ? static_cast<int16_t>(-amplitude) : amplitude;
  }
  return pcm;
}

}  // namespace

void setUp() {}
void tearDown() {}

// --- the gain itself ---------------------------------------------------------

static void unity_gain_is_an_exact_bypass() {
  TEST_ASSERT_EQUAL_INT32(tth::kGainUnityQ12, tth::dbToGainQ12(0.0f));
  // Loopback gain never attenuates.
  TEST_ASSERT_EQUAL_INT32(tth::kGainUnityQ12, tth::dbToGainQ12(-6.0f));

  for (int32_t s = -32768; s <= 32767; ++s) {
    bool limited = true;
    const int16_t out =
        tth::applyGain(static_cast<int16_t>(s), tth::kGainUnityQ12, &limited);
    if (out != s || limited) TEST_FAIL_MESSAGE("unity gain changed a sample");
  }
}

static void plus_9_db_is_a_2_82x_multiplier() {
  const int32_t g = tth::dbToGainQ12(9.0f);
  // 10^(9/20) = 2.8184 -> 11544 in Q12.
  TEST_ASSERT_INT32_WITHIN(1, 11544, g);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 9.0f, tth::gainQ12ToDb(g));
  TEST_ASSERT_EQUAL_INT16(2818, tth::applyGain(1000, g, nullptr));
  TEST_ASSERT_EQUAL_INT16(-2818, tth::applyGain(-1000, g, nullptr));

  // Below the knee it is exactly linear.
  for (int32_t s = -8000; s <= 8000; s += 7) {
    bool limited = true;
    const int16_t out = tth::applyGain(static_cast<int16_t>(s), g, &limited);
    TEST_ASSERT_EQUAL_INT32(exactAmplify(s, g), out);
    TEST_ASSERT_FALSE(limited);
  }
}

static void output_saturates_in_both_directions() {
  TEST_ASSERT_EQUAL_INT16(32767, tth::saturateToInt16(40000));
  TEST_ASSERT_EQUAL_INT16(-32768, tth::saturateToInt16(-40000));
  TEST_ASSERT_EQUAL_INT16(32767, tth::saturateToInt16(2000000000));
  TEST_ASSERT_EQUAL_INT16(-32768, tth::saturateToInt16(-2000000000));
  TEST_ASSERT_EQUAL_INT16(12345, tth::saturateToInt16(12345));

  const int32_t g12 = tth::dbToGainQ12(12.0f);
  bool limited = false;
  const int16_t high = tth::applyGain(32767, g12, &limited);
  TEST_ASSERT_TRUE(limited);
  TEST_ASSERT_TRUE(high > tth::kLimiterThreshold && high <= 32767);

  limited = false;
  const int16_t low = tth::applyGain(-32768, g12, &limited);
  TEST_ASSERT_TRUE(limited);
  TEST_ASSERT_TRUE(low < -tth::kLimiterThreshold && low >= -32768);
}

// No wrap at the MAXIMUM gain, over every possible sample: the output keeps
// the input's sign, never decreases as the input increases, and stays in
// range. A wrapped product would break all three.
static void no_integer_wrap_even_at_the_maximum_gain() {
  TEST_ASSERT_EQUAL_INT32(tth::kMaxGainQ12,
                          tth::dbToGainQ12(tth::kMaxLoopbackGainDb));
  TEST_ASSERT_EQUAL_INT32(tth::kMaxGainQ12, tth::dbToGainQ12(40.0f));
  // The bound the int32 arithmetic relies on.
  TEST_ASSERT_TRUE(static_cast<int64_t>(32768) * tth::kMaxGainQ12 <
                   static_cast<int64_t>(2147483647));

  // amplify() is exact against a 64-bit reference at the extremes...
  TEST_ASSERT_EQUAL_INT32(exactAmplify(-32768, tth::kMaxGainQ12),
                          tth::amplify(-32768, tth::kMaxGainQ12));
  TEST_ASSERT_EQUAL_INT32(exactAmplify(32767, tth::kMaxGainQ12),
                          tth::amplify(32767, tth::kMaxGainQ12));
  // ...and clamps an out-of-range gain instead of overflowing with it.
  TEST_ASSERT_EQUAL_INT32(tth::amplify(32767, tth::kMaxGainQ12),
                          tth::amplify(32767, 2147483647));

  int32_t previous = -32768;
  for (int32_t s = -32768; s <= 32767; ++s) {
    const int32_t out =
        tth::applyGain(static_cast<int16_t>(s), tth::kMaxGainQ12, nullptr);
    if (s > 0 && out <= 0) TEST_FAIL_MESSAGE("positive input came out <= 0");
    if (s < 0 && out >= 0) TEST_FAIL_MESSAGE("negative input came out >= 0");
    if (s == 0 && out != 0) TEST_FAIL_MESSAGE("zero came out non-zero");
    if (out < previous) TEST_FAIL_MESSAGE("output decreased: wrap");
    previous = out;
  }
}

static void silence_remains_silence() {
  const float gains[4] = {0.0f, 9.0f, 12.0f, 18.0f};
  for (int i = 0; i < 4; ++i) {
    TEST_ASSERT_EQUAL_INT16(0,
                            tth::applyGain(0, tth::dbToGainQ12(gains[i]),
                                           nullptr));
  }

  Rig rig(9.0f);
  rig.speak(constant(2000, 0));
  const std::vector<int16_t> out = rig.replay(kScratch);
  TEST_ASSERT_EQUAL_UINT32(2000, out.size());
  for (size_t i = 0; i < out.size(); ++i) TEST_ASSERT_EQUAL_INT16(0, out[i]);
  TEST_ASSERT_EQUAL_INT32(0, rig.mock.responseOutputPeak());
  TEST_ASSERT_EQUAL_UINT32(0, rig.mock.limitedSamples());
}

// --- where the gain applies ------------------------------------------------------

// Synthetic 24 kHz speech is never touched by the loopback gain -- and the
// player, which every future network source goes through, copies samples
// unchanged: there is no gain anywhere on that path.
static void synthetic_audio_is_unchanged_by_the_loopback_gain() {
  std::vector<int16_t> plain;
  {
    Rig rig(0.0f, tth::MockMode::Synthetic);
    rig.speak(std::vector<int16_t>());
    plain = rig.replay(kScratch);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, rig.mock.appliedGainDb());
  }
  std::vector<int16_t> boosted;
  {
    Rig rig(12.0f, tth::MockMode::Synthetic);
    rig.speak(std::vector<int16_t>());
    boosted = rig.replay(kScratch);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, rig.mock.appliedGainDb());
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, rig.mock.configuredTurnGainDb());
    TEST_ASSERT_EQUAL_UINT32(0, rig.mock.limitedSamples());
  }
  TEST_ASSERT_EQUAL_UINT32(12000, plain.size());
  TEST_ASSERT_EQUAL_UINT32(plain.size(), boosted.size());
  for (size_t i = 0; i < plain.size(); ++i) {
    TEST_ASSERT_EQUAL_INT16(plain[i], boosted[i]);
  }
}

static void the_player_itself_applies_no_gain() {
  class NullDevice : public tth::IAudioDevice {
   public:
    bool micBegin() override { return true; }
    void micEnd() override {}
    bool speakerBegin() override { return true; }
    void speakerEnd() override {}
  };
  class Speaker : public tth::ISpeakerOutput {
   public:
    std::deque<const int16_t*> held;
    uint32_t slotsOccupied() const override {
      return static_cast<uint32_t>(held.size());
    }
    bool play(const int16_t* s, uint32_t, uint32_t) override {
      held.push_back(s);
      return true;
    }
    void stop() override {}
  };
  static int16_t slots[3][64];
  NullDevice device;
  tth::AudioBus bus(device);
  Speaker speaker;
  tth::PcmPlayer player(bus, speaker, 400);
  player.begin(slots[0], slots[1], slots[2], 64);
  TEST_ASSERT_TRUE(player.openStream(tth::monoS16(24000), 0));

  const std::vector<int16_t> pcm = square(64, 1234);
  tth::AudioChunk chunk;
  chunk.samples = pcm.data();
  chunk.count = 64;
  chunk.format = tth::monoS16(24000);
  uint32_t accepted = 0;
  TEST_ASSERT_TRUE(player.submit(chunk, accepted) == tth::PlayerPush::Accepted);
  player.service(0);
  for (uint32_t i = 0; i < 64; ++i) {
    TEST_ASSERT_EQUAL_INT16(pcm[i], speaker.held.front()[i]);
  }
  TEST_ASSERT_EQUAL_INT32(1234, player.metrics().inputPeak);
}

// --- one gain per turn, continuous across chunks ----------------------------------

// The output is the same however the turn is split into chunks, and equals
// the per-sample gain applied with the turn's single gain.
static void the_gain_is_continuous_across_chunk_boundaries() {
  std::vector<int16_t> pcm(3000);
  for (uint32_t i = 0; i < pcm.size(); ++i) {
    // A ramp-modulated pattern that crosses the knee near the end.
    pcm[i] = static_cast<int16_t>(((i % 97) * 97) * ((i & 1u) ? -1 : 1));
  }
  const uint32_t sizes[3] = {kScratch, 7, 333};
  std::vector<int16_t> first;
  int32_t gain = 0;
  for (int k = 0; k < 3; ++k) {
    Rig rig(9.0f);
    rig.speak(pcm);
    const std::vector<int16_t> out = rig.replay(sizes[k]);
    TEST_ASSERT_EQUAL_UINT32(pcm.size(), out.size());
    if (k == 0) {
      first = out;
      gain = rig.mock.appliedGainQ12();
    } else {
      for (size_t i = 0; i < out.size(); ++i) {
        TEST_ASSERT_EQUAL_INT16(first[i], out[i]);
      }
    }
  }
  for (size_t i = 0; i < pcm.size(); ++i) {
    TEST_ASSERT_EQUAL_INT16(tth::applyGain(pcm[i], gain, nullptr), first[i]);
  }
}

// Peeking again (a full player ring) gives the same samples and does not
// count them twice.
static void re_peeking_does_not_reprocess_or_double_count() {
  // Quiet speech gets the full +12 dB; exactly ONE click enters the limiter.
  std::vector<int16_t> pcm = square(1000, 2000);
  pcm[100] = 32767;
  Rig rig(12.0f);
  rig.speak(pcm);
  TEST_ASSERT_EQUAL_INT32(tth::dbToGainQ12(12.0f), rig.mock.appliedGainQ12());

  tth::AudioChunk a;
  tth::AudioChunk b;
  TEST_ASSERT_TRUE(rig.mock.peekPlaybackChunk(a, 500));
  const std::vector<int16_t> copy(a.samples, a.samples + a.count);
  TEST_ASSERT_TRUE(rig.mock.peekPlaybackChunk(b, 500));
  TEST_ASSERT_EQUAL_PTR(a.samples, b.samples);
  for (uint32_t i = 0; i < b.count; ++i) {
    TEST_ASSERT_EQUAL_INT16(copy[i], b.samples[i]);
  }
  rig.mock.consumePlayback(b.count);
  rig.replay(500);
  // Peeked twice, consumed once: counted once.
  TEST_ASSERT_EQUAL_UINT32(1, rig.mock.limitedSamples());
}

// No pumping: quiet passages before and after a loud one get EXACTLY the same
// gain, even when the loud passage forces the turn's gain down.
static void one_gain_for_the_whole_turn_no_pumping() {
  std::vector<int16_t> pcm = square(1600, 500);
  const std::vector<int16_t> loud = square(1600, 14000);
  const std::vector<int16_t> quietAgain = square(1600, 500);
  pcm.insert(pcm.end(), loud.begin(), loud.end());
  pcm.insert(pcm.end(), quietAgain.begin(), quietAgain.end());

  Rig rig(9.0f);
  rig.speak(pcm);
  const std::vector<int16_t> out = rig.replay(kScratch);

  // The turn's 99.9th percentile is ~14000: 9 dB would push it past the knee,
  // so the WHOLE turn gets less -- not just the loud part.
  const int32_t g = rig.mock.appliedGainQ12();
  TEST_ASSERT_TRUE(g > tth::kGainUnityQ12);
  TEST_ASSERT_TRUE(g < tth::dbToGainQ12(9.0f));
  TEST_ASSERT_TRUE(rig.mock.appliedGainDb() < 9.0f);

  const int16_t quiet = static_cast<int16_t>(exactAmplify(500, g));
  TEST_ASSERT_EQUAL_INT16(quiet, out[0]);
  TEST_ASSERT_EQUAL_INT16(quiet, out[1598]);
  TEST_ASSERT_EQUAL_INT16(quiet, out[3200]);
  TEST_ASSERT_EQUAL_INT16(quiet, out[4798]);
  // And the loud part lands at, not above, the knee: nothing limited.
  TEST_ASSERT_EQUAL_UINT32(0, rig.mock.limitedSamples());
  TEST_ASSERT_TRUE(rig.mock.responseOutputPeak() <= tth::kLimiterThreshold);
}

// --- the limiter ----------------------------------------------------------------------

// The knee: untouched up to it, a smooth continuation just past it, strictly
// increasing, symmetric, and never at full scale -- no hard clip.
static void the_soft_limiter_rounds_off_instead_of_clipping() {
  bool limited = true;
  TEST_ASSERT_EQUAL_INT16(tth::kLimiterThreshold,
                          tth::softLimit(tth::kLimiterThreshold, &limited));
  TEST_ASSERT_FALSE(limited);

  const int16_t justPast = tth::softLimit(tth::kLimiterThreshold + 100, &limited);
  TEST_ASSERT_TRUE(limited);
  TEST_ASSERT_TRUE(justPast >= tth::kLimiterThreshold + 97 &&
                   justPast <= tth::kLimiterThreshold + 100);

  int32_t previous = tth::kLimiterThreshold;
  for (int32_t x = tth::kLimiterThreshold + 1; x <= 400000; x += 997) {
    const int32_t y = tth::softLimit(x, nullptr);
    if (y < previous) TEST_FAIL_MESSAGE("limiter output decreased");
    if (y >= 32767) TEST_FAIL_MESSAGE("limiter reached full scale");
    if (tth::softLimit(-x, nullptr) != -y) TEST_FAIL_MESSAGE("not symmetric");
    previous = y;
  }
}

// Loud input on a quiet turn: the rare clicks do not drag the turn's gain
// down (the 99.9th percentile ignores them), and they are soft-limited --
// counted, below full scale -- while the speech gets the full gain.
static void loud_clicks_are_limited_without_lowering_the_turn_gain() {
  std::vector<int16_t> pcm = square(6000, 2000);
  const uint32_t clicks[3] = {1000, 3001, 5000};
  for (int i = 0; i < 3; ++i) pcm[clicks[i]] = 32767;

  Rig rig(12.0f);
  rig.speak(pcm);
  const std::vector<int16_t> out = rig.replay(kScratch);

  const int32_t g12 = tth::dbToGainQ12(12.0f);
  TEST_ASSERT_EQUAL_INT32(g12, rig.mock.appliedGainQ12());
  TEST_ASSERT_EQUAL_UINT32(3, rig.mock.limitedSamples());
  for (int i = 0; i < 3; ++i) {
    TEST_ASSERT_TRUE(out[clicks[i]] > tth::kLimiterThreshold);
    TEST_ASSERT_TRUE(out[clicks[i]] < 32767);
  }
  TEST_ASSERT_EQUAL_INT16(static_cast<int16_t>(exactAmplify(2000, g12)), out[0]);
  TEST_ASSERT_EQUAL_INT32(32767, rig.mock.responseInputPeak());
  TEST_ASSERT_TRUE(rig.mock.responseOutputPeak() < 32767);
}

// A recording that is already hot is not boosted into the limiter at all.
static void an_already_loud_turn_gets_no_boost() {
  const std::vector<int16_t> pcm = square(4000, 29000);
  Rig rig(12.0f);
  rig.speak(pcm);
  const std::vector<int16_t> out = rig.replay(kScratch);
  TEST_ASSERT_EQUAL_INT32(tth::kGainUnityQ12, rig.mock.appliedGainQ12());
  for (size_t i = 0; i < pcm.size(); ++i) TEST_ASSERT_EQUAL_INT16(pcm[i], out[i]);
  TEST_ASSERT_EQUAL_UINT32(0, rig.mock.limitedSamples());
}

// --- metrics and latching -------------------------------------------------------------

static void the_turn_reports_its_input_and_output_peaks() {
  Rig rig(9.0f);
  rig.speak(square(2000, 3000));
  TEST_ASSERT_EQUAL_INT32(3000, rig.mock.turnInputPeak());
  TEST_ASSERT_TRUE(rig.mock.turnRobustPeak() >= 3000);
  rig.replay(kScratch);
  TEST_ASSERT_EQUAL_INT32(3000, rig.mock.responseInputPeak());
  TEST_ASSERT_EQUAL_INT32(exactAmplify(3000, rig.mock.appliedGainQ12()),
                          rig.mock.responseOutputPeak());
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 9.0f, rig.mock.appliedGainDb());
}

static void a_gain_change_mid_turn_applies_from_the_next_turn() {
  Rig rig(9.0f);
  TEST_ASSERT_TRUE(rig.mock.beginUserTurn(tth::monoS16(16000)));
  rig.mock.setLoopbackGainDb(0.0f);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 9.0f, rig.mock.configuredTurnGainDb());
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, rig.mock.loopbackGainDb());
}

static void the_histogram_finds_the_99_9th_percentile() {
  tth::TurnLevelHistogram h;
  TEST_ASSERT_EQUAL_INT32(0, h.levelCovering(999));
  std::vector<int16_t> pcm = square(10000, 1000);
  for (int i = 0; i < 5; ++i) pcm[i * 1000] = 30000;  // 0.05 %: outliers
  h.add(pcm.data(), static_cast<uint32_t>(pcm.size()));
  TEST_ASSERT_EQUAL_UINT32(10000, h.count());
  TEST_ASSERT_EQUAL_INT32(30000, h.peak());
  const int32_t robust = h.levelCovering(999);
  TEST_ASSERT_TRUE(robust >= 1000 && robust < 1280);
  TEST_ASSERT_EQUAL_INT32(30000, h.levelCovering(1000));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(unity_gain_is_an_exact_bypass);
  RUN_TEST(plus_9_db_is_a_2_82x_multiplier);
  RUN_TEST(output_saturates_in_both_directions);
  RUN_TEST(no_integer_wrap_even_at_the_maximum_gain);
  RUN_TEST(silence_remains_silence);
  RUN_TEST(synthetic_audio_is_unchanged_by_the_loopback_gain);
  RUN_TEST(the_player_itself_applies_no_gain);
  RUN_TEST(the_gain_is_continuous_across_chunk_boundaries);
  RUN_TEST(re_peeking_does_not_reprocess_or_double_count);
  RUN_TEST(one_gain_for_the_whole_turn_no_pumping);
  RUN_TEST(the_soft_limiter_rounds_off_instead_of_clipping);
  RUN_TEST(loud_clicks_are_limited_without_lowering_the_turn_gain);
  RUN_TEST(an_already_loud_turn_gets_no_boost);
  RUN_TEST(the_turn_reports_its_input_and_output_peaks);
  RUN_TEST(a_gain_change_mid_turn_applies_from_the_next_turn);
  RUN_TEST(the_histogram_finds_the_99_9th_percentile);
  return UNITY_END();
}
