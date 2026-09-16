// Host-side tests for the AudioBus ownership boundary.
//
// These encode the project's half-duplex decisions as executable checks:
//   1. mic and speaker are never installed at the same time;
//   2. AudioBus is the only thing that begins/ends them;
//   3. end() is NEVER called for a peripheral that was not successfully
//      begun (the "I2S port 1 has not installed" bug seen in Phase 0);
//   4. when the speaker must yield to the mic, it is ended BEFORE the mic
//      starts, never after.

#include <string>

#include <unity.h>

#include "tth/AudioBus.h"

namespace {

// Records the exact sequence of hardware calls so ordering can be asserted,
// not just the final state. "+" is a successful begin, "-" an end, "x" a
// begin that was refused.
class FakeAudioDevice : public tth::IAudioDevice {
 public:
  std::string log;
  bool micBeginResult = true;
  bool speakerBeginResult = true;
  int micInstalls = 0;
  int speakerInstalls = 0;

  bool micBegin() override {
    log += micBeginResult ? "mic+ " : "micx ";
    if (micBeginResult) ++micInstalls;
    return micBeginResult;
  }

  void micEnd() override {
    log += "mic- ";
    --micInstalls;
  }

  bool speakerBegin() override {
    log += speakerBeginResult ? "spk+ " : "spkx ";
    if (speakerBeginResult) ++speakerInstalls;
    return speakerBeginResult;
  }

  void speakerEnd() override {
    log += "spk- ";
    --speakerInstalls;
  }

  // Goes negative if an end() was ever issued without a matching successful
  // begin() -- i.e. the unmatched-uninstall bug.
  bool balanced() const { return micInstalls >= 0 && speakerInstalls >= 0; }

  bool exclusive() const { return !(micInstalls > 0 && speakerInstalls > 0); }
};

}  // namespace

void setUp() {}
void tearDown() {}

static void starts_unowned_and_touches_no_hardware() {
  FakeAudioDevice device;
  tth::AudioBus bus(device);

  TEST_ASSERT_EQUAL(tth::AudioOwner::None, bus.owner());
  TEST_ASSERT_EQUAL_STRING("", device.log.c_str());
}

// The Phase 0 bug, pinned: releasing a bus nobody owns must not uninstall
// anything.
static void release_when_unowned_is_a_no_op() {
  FakeAudioDevice device;
  tth::AudioBus bus(device);

  bus.releaseAll();
  bus.releaseAll();

  TEST_ASSERT_EQUAL_STRING("", device.log.c_str());
  TEST_ASSERT_TRUE(device.balanced());
}

static void acquire_mic_from_idle_starts_only_the_mic() {
  FakeAudioDevice device;
  tth::AudioBus bus(device);

  TEST_ASSERT_TRUE(bus.acquireMic());

  TEST_ASSERT_EQUAL_STRING("mic+ ", device.log.c_str());
  TEST_ASSERT_TRUE(bus.micOwns());
  TEST_ASSERT_EQUAL_UINT32(1, bus.transitions());
}

static void acquiring_the_same_owner_twice_is_idempotent() {
  FakeAudioDevice device;
  tth::AudioBus bus(device);

  TEST_ASSERT_TRUE(bus.acquireMic());
  TEST_ASSERT_TRUE(bus.acquireMic());

  TEST_ASSERT_EQUAL_STRING("mic+ ", device.log.c_str());
  TEST_ASSERT_EQUAL_UINT32(1, bus.transitions());
}

// Ordering is the whole point: speaker down, THEN mic up.
static void switching_to_mic_ends_the_speaker_first() {
  FakeAudioDevice device;
  tth::AudioBus bus(device);

  TEST_ASSERT_TRUE(bus.acquireSpeaker());
  TEST_ASSERT_TRUE(bus.acquireMic());

  TEST_ASSERT_EQUAL_STRING("spk+ spk- mic+ ", device.log.c_str());
  TEST_ASSERT_TRUE(bus.micOwns());
  TEST_ASSERT_TRUE(device.exclusive());
}

static void switching_to_speaker_ends_the_mic_first() {
  FakeAudioDevice device;
  tth::AudioBus bus(device);

  TEST_ASSERT_TRUE(bus.acquireMic());
  TEST_ASSERT_TRUE(bus.acquireSpeaker());

  TEST_ASSERT_EQUAL_STRING("mic+ mic- spk+ ", device.log.c_str());
  TEST_ASSERT_TRUE(bus.speakerOwns());
  TEST_ASSERT_TRUE(device.exclusive());
}

// A failed acquire must leave the bus unowned rather than silently restoring
// the peripheral that was just torn down.
static void failed_mic_acquire_leaves_the_bus_unowned() {
  FakeAudioDevice device;
  tth::AudioBus bus(device);

  TEST_ASSERT_TRUE(bus.acquireSpeaker());
  device.micBeginResult = false;

  TEST_ASSERT_FALSE(bus.acquireMic());

  TEST_ASSERT_EQUAL(tth::AudioOwner::None, bus.owner());
  TEST_ASSERT_EQUAL_STRING("spk+ spk- micx ", device.log.c_str());
  TEST_ASSERT_EQUAL_UINT32(1, bus.failedAcquires());
  TEST_ASSERT_TRUE(device.balanced());
}

// After a failed acquire, releaseAll() must still not uninstall anything.
static void release_after_failed_acquire_is_a_no_op() {
  FakeAudioDevice device;
  tth::AudioBus bus(device);
  device.speakerBeginResult = false;

  TEST_ASSERT_FALSE(bus.acquireSpeaker());
  bus.releaseAll();

  TEST_ASSERT_EQUAL_STRING("spkx ", device.log.c_str());
  TEST_ASSERT_TRUE(device.balanced());
}

static void release_ends_exactly_the_installed_peripheral() {
  FakeAudioDevice device;
  tth::AudioBus bus(device);

  bus.acquireMic();
  bus.releaseAll();
  TEST_ASSERT_EQUAL_STRING("mic+ mic- ", device.log.c_str());

  bus.acquireSpeaker();
  bus.releaseAll();
  TEST_ASSERT_EQUAL_STRING("mic+ mic- spk+ spk- ", device.log.c_str());

  TEST_ASSERT_EQUAL(tth::AudioOwner::None, bus.owner());
  TEST_ASSERT_TRUE(device.balanced());
}

// A full push-to-talk turn plus a barge-in, which is the sequence Phase 5
// will actually drive.
static void a_turn_with_barge_in_stays_exclusive_throughout() {
  FakeAudioDevice device;
  tth::AudioBus bus(device);

  bus.acquireMic();      // child holds the button
  bus.acquireSpeaker();  // robot answers
  bus.acquireMic();      // child interrupts
  bus.releaseAll();      // turn abandoned

  TEST_ASSERT_EQUAL_STRING("mic+ mic- spk+ spk- mic+ mic- ",
                           device.log.c_str());
  TEST_ASSERT_TRUE(device.exclusive());
  TEST_ASSERT_TRUE(device.balanced());
  TEST_ASSERT_EQUAL_UINT32(3, bus.transitions());
  TEST_ASSERT_EQUAL_UINT32(0, bus.failedAcquires());
}

static void owner_names_are_stable() {
  TEST_ASSERT_EQUAL_STRING("none", tth::toString(tth::AudioOwner::None));
  TEST_ASSERT_EQUAL_STRING("mic", tth::toString(tth::AudioOwner::Mic));
  TEST_ASSERT_EQUAL_STRING("speaker", tth::toString(tth::AudioOwner::Speaker));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(starts_unowned_and_touches_no_hardware);
  RUN_TEST(release_when_unowned_is_a_no_op);
  RUN_TEST(acquire_mic_from_idle_starts_only_the_mic);
  RUN_TEST(acquiring_the_same_owner_twice_is_idempotent);
  RUN_TEST(switching_to_mic_ends_the_speaker_first);
  RUN_TEST(switching_to_speaker_ends_the_mic_first);
  RUN_TEST(failed_mic_acquire_leaves_the_bus_unowned);
  RUN_TEST(release_after_failed_acquire_is_a_no_op);
  RUN_TEST(release_ends_exactly_the_installed_peripheral);
  RUN_TEST(a_turn_with_barge_in_stays_exclusive_throughout);
  RUN_TEST(owner_names_are_stable);
  return UNITY_END();
}
