// Host-side tests for the haptic pulse: one non-blocking 120 ms pulse, fired
// when the speaker first ACCEPTS audio -- not when speech is announced, and
// not continuously through the response.

#include <string.h>

#include <deque>

#include <unity.h>

#include "tth/AudioBus.h"
#include "tth/AudioFormat.h"
#include "tth/HapticPattern.h"
#include "tth/HapticPulse.h"
#include "tth/PcmPlayer.h"

namespace {

const uint32_t kPulseMs = 120;

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

// What App does each loop: service the player, pulse on first audio, poll the
// pulse. Returns how many times the motor was switched on and off.
struct Rig {
  FakeAudioDevice device;
  tth::AudioBus bus;
  Speaker speaker;
  tth::PcmPlayer player;
  tth::HapticPulse pulse;
  uint32_t on = 0;
  uint32_t off = 0;

  Rig() : bus(device), player(bus, speaker, 400), pulse(kPulseMs) {
    player.begin(g_slots[0], g_slots[1], g_slots[2], kSlot);
  }

  void loop(uint32_t nowMs) {
    player.service(nowMs);
    if (player.consumeFirstAudio() && pulse.start(nowMs)) ++on;
    if (pulse.poll(nowMs)) ++off;
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

static void a_pulse_switches_on_then_off_exactly_once() {
  tth::HapticPulse pulse(kPulseMs);

  TEST_ASSERT_TRUE(pulse.start(1000));
  TEST_ASSERT_TRUE(pulse.isActive());

  TEST_ASSERT_FALSE(pulse.poll(1000 + kPulseMs - 1));
  TEST_ASSERT_TRUE(pulse.isActive());

  TEST_ASSERT_TRUE(pulse.poll(1000 + kPulseMs));
  TEST_ASSERT_FALSE(pulse.isActive());
  TEST_ASSERT_FALSE(pulse.poll(1000 + kPulseMs + 1));
  TEST_ASSERT_FALSE(pulse.poll(5000));
  TEST_ASSERT_EQUAL_UINT32(1, pulse.pulses());
}

static void a_running_pulse_is_not_restarted_or_extended() {
  tth::HapticPulse pulse(kPulseMs);
  TEST_ASSERT_TRUE(pulse.start(1000));
  TEST_ASSERT_FALSE(pulse.start(1100));
  // Still ends 120 ms after the FIRST start.
  TEST_ASSERT_TRUE(pulse.poll(1000 + kPulseMs));
  TEST_ASSERT_EQUAL_UINT32(1, pulse.pulses());
}

static void a_pulse_across_the_millis_rollover_still_lasts_120ms() {
  tth::HapticPulse pulse(kPulseMs);
  const uint32_t nearMax = 0xFFFFFFC0u;
  TEST_ASSERT_TRUE(pulse.start(nearMax));
  TEST_ASSERT_FALSE(pulse.poll(nearMax + kPulseMs - 1));
  TEST_ASSERT_TRUE(pulse.poll(nearMax + kPulseMs));
}

// A whole response of twenty chunks produces ONE pulse -- no continuous
// vibration through the response.
static void one_response_produces_exactly_one_pulse() {
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

  TEST_ASSERT_EQUAL_UINT32(1, rig.on);
  TEST_ASSERT_EQUAL_UINT32(1, rig.off);
}

// The pulse follows the first ACCEPTED audio, not the announcement: opening
// the stream alone, or queueing into the ring, does not vibrate.
static void no_pulse_until_the_speaker_accepts_audio() {
  Rig rig;
  TEST_ASSERT_TRUE(rig.player.openStream(tth::monoS16(24000), 0));
  rig.speaker.refuse = true;
  rig.feed();
  for (uint32_t t = 0; t < 500; t += 10) rig.loop(t);
  TEST_ASSERT_EQUAL_UINT32(0, rig.on);

  rig.speaker.refuse = false;
  rig.loop(500);
  TEST_ASSERT_EQUAL_UINT32(1, rig.on);
}

static void every_new_response_gets_its_own_pulse() {
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
  TEST_ASSERT_EQUAL_UINT32(3, rig.on);
  TEST_ASSERT_EQUAL_UINT32(3, rig.off);
}

// --- Step 6.1: the refused-press pattern (two short pulses) ------------------

namespace {
const uint16_t kDenied[3] = {40, 80, 40};
}  // namespace

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
  RUN_TEST(a_pulse_switches_on_then_off_exactly_once);
  RUN_TEST(a_running_pulse_is_not_restarted_or_extended);
  RUN_TEST(a_pulse_across_the_millis_rollover_still_lasts_120ms);
  RUN_TEST(one_response_produces_exactly_one_pulse);
  RUN_TEST(no_pulse_until_the_speaker_accepts_audio);
  RUN_TEST(every_new_response_gets_its_own_pulse);
  return UNITY_END();
}
