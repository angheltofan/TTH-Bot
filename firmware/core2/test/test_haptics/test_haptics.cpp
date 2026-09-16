// Host-side tests for haptics.
//
// The motor has ONE use: the two-short-pulse pattern when an action is
// refused. There is no vibration when the robot starts speaking (the former
// 120 ms first-audio pulse was removed by product decision) -- while the
// player still reports the first accepted audio exactly once per stream,
// because the latency log and the M4 memory point use it.

#include <string.h>

#include <deque>

#include <unity.h>

#include "tth/AudioBus.h"
#include "tth/AudioFormat.h"
#include "tth/HapticPattern.h"
#include "tth/PcmPlayer.h"

namespace {

class FakeAudioDevice : public tth::IAudioDevice {
 public:
  bool micBegin() override { return true; }
  void micEnd() override {}
  bool speakerBegin() override { return true; }
  void speakerEnd() override {}
};

class Speaker : public tth::ISpeakerOutput {
 public:
  std::deque<const int16_t*> held;
  bool refuse = false;
  uint32_t slotsOccupied() const override {
    return static_cast<uint32_t>(held.size());
  }
  bool play(const int16_t* samples, uint32_t, uint32_t) override {
    if (held.size() >= tth::kSpeakerQueueDepth) {
      TEST_FAIL_MESSAGE("play() with both speaker slots occupied");
    }
    if (refuse) return false;
    held.push_back(samples);
    return true;
  }
  void stop() override {}
};

const uint32_t kSlot = 8;
int16_t g_slots[3][kSlot];
int16_t g_pcm[kSlot];

const uint16_t kDenied[3] = {40, 80, 40};

// What App does each loop during playback: service the player, log the first
// accepted audio, poll haptics. The only haptic output App has is the refused
// pattern, which playback never starts.
struct Rig {
  FakeAudioDevice device;
  tth::AudioBus bus;
  Speaker speaker;
  tth::PcmPlayer player;
  tth::HapticPattern haptics;
  uint32_t firstAudio = 0;
  uint32_t motorChanges = 0;

  Rig() : bus(device), player(bus, speaker, 400), haptics(kDenied, 3) {
    player.begin(g_slots[0], g_slots[1], g_slots[2], kSlot);
  }

  void loop(uint32_t nowMs) {
    player.service(nowMs);
    if (player.consumeFirstAudio()) ++firstAudio;
    if (haptics.poll(nowMs) != tth::HapticPattern::Motor::NoChange) ++motorChanges;
  }

  void feed() {
    tth::AudioChunk chunk;
    chunk.samples = g_pcm;
    chunk.count = kSlot;
    chunk.format = tth::monoS16(24000);
    uint32_t accepted = 0;
    player.submit(chunk, accepted);
  }
};

}  // namespace

void setUp() {}
void tearDown() {}

// --- playback never vibrates ------------------------------------------------------

// A whole response of twenty chunks: the first audio is reported once, and
// the motor is never switched.
static void a_whole_response_never_vibrates() {
  Rig rig;
  TEST_ASSERT_TRUE(rig.player.openStream(tth::monoS16(24000), 0));
  uint32_t t = 0;
  for (int chunk = 0; chunk < 20; ++chunk) {
    rig.feed();
    for (int step = 0; step < 4; ++step) {
      rig.loop(t);
      t += 10;
    }
    rig.speaker.held.clear();
  }
  rig.loop(t + 1000);
  TEST_ASSERT_EQUAL_UINT32(1, rig.firstAudio);
  TEST_ASSERT_EQUAL_UINT32(0, rig.haptics.starts());
  TEST_ASSERT_EQUAL_UINT32(0, rig.motorChanges);
}

// First audio is still reported only once the speaker ACCEPTS audio (speaker
// startup unchanged), and still without any vibration.
static void first_audio_is_reported_when_the_speaker_accepts_without_vibration() {
  Rig rig;
  TEST_ASSERT_TRUE(rig.player.openStream(tth::monoS16(24000), 0));
  rig.speaker.refuse = true;
  rig.feed();
  for (uint32_t t = 0; t < 500; t += 10) rig.loop(t);
  TEST_ASSERT_EQUAL_UINT32(0, rig.firstAudio);
  rig.speaker.refuse = false;
  rig.loop(500);
  TEST_ASSERT_EQUAL_UINT32(1, rig.firstAudio);
  TEST_ASSERT_EQUAL_UINT32(0, rig.haptics.starts());
}

// Every response reports its own first audio; the refused-pattern counter
// (the heartbeat's hapticsRefused) is not moved by any of them.
static void responses_do_not_move_the_refused_counter() {
  Rig rig;
  for (int response = 0; response < 3; ++response) {
    const uint32_t base = static_cast<uint32_t>(response) * 1000u;
    TEST_ASSERT_TRUE(rig.player.openStream(tth::monoS16(24000), base));
    rig.feed();
    rig.loop(base);
    rig.player.markEndOfStream(base);
    rig.speaker.held.clear();
    rig.loop(base + 200);
    TEST_ASSERT_TRUE(rig.player.release());
  }
  TEST_ASSERT_EQUAL_UINT32(3, rig.firstAudio);
  TEST_ASSERT_EQUAL_UINT32(0, rig.haptics.starts());
  // A refused action is still counted, exactly once per pattern.
  TEST_ASSERT_TRUE(rig.haptics.start(5000));
  TEST_ASSERT_EQUAL_UINT32(1, rig.haptics.starts());
}

// --- the refused pattern (two short pulses), unchanged ------------------------------

static void the_denied_pattern_is_two_short_pulses() {
  using Motor = tth::HapticPattern::Motor;
  tth::HapticPattern pattern(kDenied, 3);
  TEST_ASSERT_TRUE(pattern.start(1000));
  TEST_ASSERT_TRUE(pattern.poll(1039) == Motor::NoChange);
  TEST_ASSERT_TRUE(pattern.poll(1040) == Motor::Off);
  TEST_ASSERT_TRUE(pattern.poll(1119) == Motor::NoChange);
  TEST_ASSERT_TRUE(pattern.poll(1120) == Motor::On);
  TEST_ASSERT_TRUE(pattern.poll(1159) == Motor::NoChange);
  TEST_ASSERT_TRUE(pattern.poll(1160) == Motor::Off);
  TEST_ASSERT_FALSE(pattern.isActive());
  TEST_ASSERT_TRUE(pattern.poll(2000) == Motor::NoChange);
}

static void a_running_pattern_is_not_restarted() {
  tth::HapticPattern pattern(kDenied, 3);
  TEST_ASSERT_TRUE(pattern.start(0));
  TEST_ASSERT_FALSE(pattern.start(10));
  pattern.poll(200);
  TEST_ASSERT_TRUE(pattern.start(300));
  TEST_ASSERT_EQUAL_UINT32(2, pattern.starts());
}

// A stalled loop skips the missed steps and still ends with the motor off.
static void a_slow_loop_still_ends_with_the_motor_off() {
  tth::HapticPattern pattern(kDenied, 3);
  pattern.start(0);
  TEST_ASSERT_TRUE(pattern.poll(5000) == tth::HapticPattern::Motor::Off);
  TEST_ASSERT_FALSE(pattern.isActive());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(the_denied_pattern_is_two_short_pulses);
  RUN_TEST(a_running_pattern_is_not_restarted);
  RUN_TEST(a_slow_loop_still_ends_with_the_motor_off);
  RUN_TEST(a_whole_response_never_vibrates);
  RUN_TEST(first_audio_is_reported_when_the_speaker_accepts_without_vibration);
  RUN_TEST(responses_do_not_move_the_refused_counter);
  return UNITY_END();
}
