// Host-side tests for barge-in: pressing push-to-talk while the robot speaks.
//
// The ORDER is the requirement:
//   1 stop accepting playback  2 stop + drain the speaker  3 cancel the source
//   4 release the speaker      5 start the microphone      6 LISTENING
//
// First with recording fakes (order, pending, failure paths), then with the
// real player, mock source, turn buffer and AudioBus wired together, to prove
// the real components obey it -- including that the microphone is never begun
// while the speaker is installed.

#include <string.h>

#include <deque>
#include <string>
#include <vector>

#include <unity.h>

#include "tth/AudioBus.h"
#include "tth/AudioFormat.h"
#include "tth/BargeIn.h"
#include "tth/LocalMockTurnSource.h"
#include "tth/PcmPlayer.h"
#include "tth/StageTimer.h"
#include "tth/TurnBuffer.h"
#include "tth/TurnStreamer.h"

namespace {

class RecordingOps : public tth::IBargeInOps {
 public:
  std::string log;
  bool quiet = false;
  bool held = true;
  bool captureResult = true;

  void stopAcceptingPlayback() override { log += "accept- "; }
  void stopSpeaker(uint32_t) override { log += "spk.stop "; }
  bool speakerQuiet() override {
    log += quiet ? "quiet " : "busy ";
    return quiet;
  }
  void cancelTurnSource() override { log += "src.cancel "; }
  void releaseSpeaker() override { log += "spk.release "; }
  bool startCapture(uint32_t) override {
    log += captureResult ? "mic.start+ " : "mic.startx ";
    return captureResult;
  }
  bool stillHeld() const override { return held; }
};

class FakeClock : public tth::IStageTimer {
 public:
  uint32_t now = 0;
  std::vector<std::string> stages;
  uint32_t lastTotal = 0;
  uint32_t nowMicros() const override { return now; }
  void report(const char* stage, uint32_t elapsed) override {
    stages.push_back(stage);
    if (std::string(stage) == "barge.total") lastTotal = elapsed;
  }
};

}  // namespace

void setUp() {}
void tearDown() {}

// --- ordering, with fakes ----------------------------------------------------

static void the_steps_run_in_the_required_order() {
  RecordingOps ops;
  tth::BargeIn barge(ops);

  TEST_ASSERT_TRUE(barge.begin(1000));
  TEST_ASSERT_EQUAL_STRING("accept- spk.stop ", ops.log.c_str());

  ops.quiet = true;
  TEST_ASSERT_TRUE(barge.poll(1010) == tth::BargeInResult::Listening);
  TEST_ASSERT_EQUAL_STRING(
      "accept- spk.stop quiet src.cancel spk.release mic.start+ ",
      ops.log.c_str());
  TEST_ASSERT_FALSE(barge.isActive());
  TEST_ASSERT_EQUAL_UINT32(10, barge.lastDrainMs());
}

// Nothing after step 2 happens until the speaker is quiet, however many loops
// that takes.
static void the_microphone_waits_for_the_speaker_to_go_quiet() {
  RecordingOps ops;
  tth::BargeIn barge(ops);
  barge.begin(0);

  for (uint32_t t = 1; t < 10; ++t) {
    TEST_ASSERT_TRUE(barge.poll(t) == tth::BargeInResult::Pending);
  }
  TEST_ASSERT_TRUE(ops.log.find("src.cancel") == std::string::npos);
  TEST_ASSERT_TRUE(ops.log.find("mic.start") == std::string::npos);
  TEST_ASSERT_TRUE(barge.isActive());

  ops.quiet = true;
  TEST_ASSERT_TRUE(barge.poll(10) == tth::BargeInResult::Listening);
}

static void a_microphone_that_fails_to_start_is_reported_not_hidden() {
  RecordingOps ops;
  ops.quiet = true;
  ops.captureResult = false;
  tth::BargeIn barge(ops);
  barge.begin(0);

  TEST_ASSERT_TRUE(barge.poll(1) == tth::BargeInResult::Failed);
  // The speaker was still released first: nothing is left owned.
  TEST_ASSERT_TRUE(ops.log.find("spk.release") < ops.log.find("mic.startx"));
}

// Released before the microphone could start: no capture nobody is holding.
static void letting_go_during_the_drain_skips_the_microphone() {
  RecordingOps ops;
  tth::BargeIn barge(ops);
  barge.begin(0);
  barge.poll(1);

  ops.held = false;
  ops.quiet = true;
  TEST_ASSERT_TRUE(barge.poll(2) == tth::BargeInResult::ReleasedEarly);
  TEST_ASSERT_TRUE(ops.log.find("mic.start") == std::string::npos);
  TEST_ASSERT_TRUE(ops.log.find("spk.release") != std::string::npos);
}

static void a_second_press_during_a_barge_in_is_refused() {
  RecordingOps ops;
  tth::BargeIn barge(ops);
  TEST_ASSERT_TRUE(barge.begin(0));
  TEST_ASSERT_FALSE(barge.begin(1));
  TEST_ASSERT_EQUAL_UINT32(1, barge.count());
}

static void poll_does_nothing_when_idle() {
  RecordingOps ops;
  tth::BargeIn barge(ops);
  TEST_ASSERT_TRUE(barge.poll(0) == tth::BargeInResult::Idle);
  TEST_ASSERT_EQUAL_STRING("", ops.log.c_str());
}

// Every step is a named stage, and the whole transition is measured.
static void each_step_and_the_total_are_measured() {
  RecordingOps ops;
  FakeClock clock;
  tth::BargeIn barge(ops);
  barge.setStageTimer(&clock);

  clock.now = 1000;
  barge.begin(0);
  clock.now = 1500;
  ops.quiet = true;
  barge.poll(1);

  const char* expected[] = {"barge.stopAccepting", "barge.stopSpeaker",
                            "barge.cancelSource", "barge.releaseSpeaker",
                            "barge.startCapture", "barge.total"};
  TEST_ASSERT_EQUAL_UINT32(6, clock.stages.size());
  for (int i = 0; i < 6; ++i) {
    TEST_ASSERT_EQUAL_STRING(expected[i], clock.stages[i].c_str());
  }
  TEST_ASSERT_EQUAL_UINT32(500, clock.lastTotal);
  TEST_ASSERT_EQUAL_UINT32(500, barge.lastTotalMicros());
}

// --- the real components ------------------------------------------------------

namespace {

const uint32_t kCapacity = 8000;
const uint32_t kSlot = 480;
int16_t g_storage[kCapacity];
int16_t g_slots[3][kSlot];
int16_t g_scratch[kSlot];

class LoggingDevice : public tth::IAudioDevice {
 public:
  std::string* log = nullptr;
  bool micBegin() override {
    *log += "bus.mic+ ";
    return true;
  }
  void micEnd() override { *log += "bus.mic- "; }
  bool speakerBegin() override {
    *log += "bus.spk+ ";
    return true;
  }
  void speakerEnd() override { *log += "bus.spk- "; }
};

class Speaker : public tth::ISpeakerOutput {
 public:
  std::string* log = nullptr;
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
  void stop() override { *log += "spk.stop "; }
};

// The same steps App performs, bound to the real components.
class RealOps : public tth::IBargeInOps {
 public:
  tth::PcmPlayer* player = nullptr;
  tth::LocalMockTurnSource* source = nullptr;
  tth::TurnBuffer* buffer = nullptr;
  tth::AudioBus* bus = nullptr;
  std::string* log = nullptr;
  bool accepting = true;

  void stopAcceptingPlayback() override {
    accepting = false;
    *log += "accept- ";
  }
  void stopSpeaker(uint32_t nowMs) override {
    player->cancel(nowMs, tth::PlaybackEnd::Cancelled);
  }
  bool speakerQuiet() override {
    return player->isDrained() || player->isIdle();
  }
  void cancelTurnSource() override {
    source->cancel();
    *log += "src.cancel ";
  }
  void releaseSpeaker() override { player->release(); }
  bool startCapture(uint32_t) override {
    // What CaptureController::start() needs: the buffer free to reset, then
    // the microphone.
    if (!buffer->reset()) return false;
    return bus->acquireMic();
  }
  bool stillHeld() const override { return true; }
};

tth::MockConfig mockConfig() {
  tth::MockConfig c;
  c.captureFormat = tth::monoS16(16000);
  c.assistantFormat = tth::monoS16(24000);
  c.thinkMs = 600;
  c.synthDurationMs = 1000;
  c.busyWindowMs = 0;
  c.busyPeriodMs = 0;
  return c;
}

}  // namespace

static void the_real_components_barge_in_safely_mid_loopback() {
  std::string log;
  LoggingDevice device;
  device.log = &log;
  tth::AudioBus bus(device);
  Speaker speaker;
  speaker.log = &log;
  tth::PcmPlayer player(bus, speaker, 400);
  player.begin(g_slots[0], g_slots[1], g_slots[2], kSlot);

  tth::TurnBuffer buffer;
  memset(g_storage, 0, sizeof(g_storage));
  buffer.attach(g_storage, kCapacity);
  tth::LocalMockTurnSource source(buffer, mockConfig());
  source.attachSynthScratch(g_scratch, kSlot);
  tth::TurnStreamer streamer(320, 100);

  // A recorded user turn, streamed to the mock.
  for (uint32_t i = 0; i < 4000; ++i) g_storage[i] = static_cast<int16_t>(i);
  int16_t ramp[4000];
  for (uint32_t i = 0; i < 4000; ++i) ramp[i] = static_cast<int16_t>(i);
  buffer.append(ramp, 4000);
  source.poll(1000);
  TEST_ASSERT_TRUE(streamer.begin(buffer, source, tth::monoS16(16000)));
  TEST_ASSERT_TRUE(streamer.service(true) == tth::StreamStatus::Finished);

  // The response starts playing.
  source.poll(1600);
  tth::TurnEvent event;
  TEST_ASSERT_TRUE(source.nextEvent(event));
  TEST_ASSERT_TRUE(event.type == tth::TurnEventType::SpeechStart);
  TEST_ASSERT_TRUE(player.openStream(event.format, 1600));
  for (int i = 0; i < 3; ++i) {
    tth::AudioChunk chunk;
    TEST_ASSERT_TRUE(source.peekPlaybackChunk(chunk, player.slotSamples()));
    uint32_t accepted = 0;
    TEST_ASSERT_TRUE(player.submit(chunk, accepted) ==
                     tth::PlayerPush::Accepted);
    source.consumePlayback(accepted);
  }
  player.service(1600);
  TEST_ASSERT_EQUAL_UINT32(2, speaker.held.size());
  TEST_ASSERT_TRUE(source.holdsLease());

  // BARGE-IN.
  RealOps ops;
  ops.player = &player;
  ops.source = &source;
  ops.buffer = &buffer;
  ops.bus = &bus;
  ops.log = &log;
  tth::BargeIn barge(ops);
  log.clear();

  TEST_ASSERT_TRUE(barge.begin(2000));
  player.service(2001);
  TEST_ASSERT_TRUE(barge.poll(2001) == tth::BargeInResult::Pending);
  TEST_ASSERT_TRUE(bus.speakerOwns());  // not released while still playing

  speaker.held.clear();  // the speaker task lets go
  player.service(2010);
  TEST_ASSERT_TRUE(barge.poll(2010) == tth::BargeInResult::Listening);

  // Exact hardware order: speaker stopped, source cancelled, speaker ended,
  // and only then the microphone begun.
  TEST_ASSERT_EQUAL_STRING("accept- spk.stop src.cancel bus.spk- bus.mic+ ",
                           log.c_str());
  TEST_ASSERT_TRUE(bus.micOwns());

  // Nothing from the abandoned response is delivered afterwards.
  TEST_ASSERT_FALSE(source.nextEvent(event));
  tth::AudioChunk late;
  TEST_ASSERT_FALSE(source.peekPlaybackChunk(late, player.slotSamples()));
  TEST_ASSERT_FALSE(source.holdsLease());
  TEST_ASSERT_TRUE(player.isIdle());
}

// If a response's lease were somehow still held, the microphone must not
// start over the audio being played.
static void the_microphone_cannot_start_over_a_buffer_still_being_played() {
  std::string log;
  LoggingDevice device;
  device.log = &log;
  tth::AudioBus bus(device);
  Speaker speaker;
  speaker.log = &log;
  tth::PcmPlayer player(bus, speaker, 400);
  player.begin(g_slots[0], g_slots[1], g_slots[2], kSlot);
  tth::TurnBuffer buffer;
  buffer.attach(g_storage, kCapacity);
  tth::LocalMockTurnSource source(buffer, mockConfig());

  TEST_ASSERT_TRUE(buffer.retain());  // an outstanding reader

  RealOps ops;
  ops.player = &player;
  ops.source = &source;
  ops.buffer = &buffer;
  ops.bus = &bus;
  ops.log = &log;
  tth::BargeIn barge(ops);
  barge.begin(0);
  TEST_ASSERT_TRUE(barge.poll(1) == tth::BargeInResult::Failed);
  TEST_ASSERT_TRUE(log.find("bus.mic+") == std::string::npos);
  buffer.release();
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(the_steps_run_in_the_required_order);
  RUN_TEST(the_microphone_waits_for_the_speaker_to_go_quiet);
  RUN_TEST(a_microphone_that_fails_to_start_is_reported_not_hidden);
  RUN_TEST(letting_go_during_the_drain_skips_the_microphone);
  RUN_TEST(a_second_press_during_a_barge_in_is_refused);
  RUN_TEST(poll_does_nothing_when_idle);
  RUN_TEST(each_step_and_the_total_are_measured);
  RUN_TEST(the_real_components_barge_in_safely_mid_loopback);
  RUN_TEST(the_microphone_cannot_start_over_a_buffer_still_being_played);
  return UNITY_END();
}
