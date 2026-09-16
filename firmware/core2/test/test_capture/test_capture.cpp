// Host-side tests for CaptureController: the full life of one push-to-talk
// recording, including every way it can end badly.

#include <string.h>

#include <string>

#include <unity.h>

#include "tth/AudioBus.h"
#include "tth/CaptureController.h"
#include "tth/IAudioCapture.h"

namespace {

const uint32_t kCapacity = 1024;   // samples: an exact multiple of kChunk
const uint32_t kChunk = 64;        // samples per read
const uint32_t kMaxDurationMs = 45000;
const uint32_t kDrainTimeoutMs = 250;

int16_t g_turnStorage[kCapacity];
int16_t g_chunkStorage[kChunk];

// Records the exact sequence of hardware calls, so ordering can be asserted.
class FakeAudioDevice : public tth::IAudioDevice {
 public:
  std::string log;
  bool micBeginResult = true;

  bool micBegin() override {
    log += micBeginResult ? "bus.mic+ " : "bus.micx ";
    return micBeginResult;
  }
  void micEnd() override { log += "bus.mic- "; }
  bool speakerBegin() override {
    log += "bus.spk+ ";
    return true;
  }
  void speakerEnd() override { log += "bus.spk- "; }
};

class FakeCapture : public tth::IAudioCapture {
 public:
  std::string* log = nullptr;

  bool startResult = true;
  // Samples returned per readChunk(); 0 means "nothing ready yet".
  int samplesPerRead = static_cast<int>(kChunk);
  // After this many successful reads, fail. Negative = never fail.
  int failAfterReads = -1;
  int16_t sampleValue = 1000;
  // When set, chunks alternate sign so DC removal leaves real signal behind.
  // A constant-valued chunk is, correctly, silence once centred.
  bool alternate = false;

  int reads = 0;
  int startCalls = 0;
  int stopCalls = 0;
  int finishCalls = 0;
  uint32_t fakeMaxReadMicros = 0;

  bool startCapture() override {
    ++startCalls;
    if (log) *log += startResult ? "cap.start+ " : "cap.startx ";
    return startResult;
  }

  int readChunk(int16_t* dest, uint32_t maxSamples) override {
    if (failAfterReads >= 0 && reads >= failAfterReads) return -1;
    ++reads;
    const uint32_t count =
        (static_cast<uint32_t>(samplesPerRead) < maxSamples)
            ? static_cast<uint32_t>(samplesPerRead)
            : maxSamples;
    for (uint32_t i = 0; i < count; ++i) {
      dest[i] = (alternate && ((i & 1u) != 0u))
                    ? static_cast<int16_t>(-sampleValue)
                    : sampleValue;
    }
    return static_cast<int>(count);
  }

  bool drained = true;  // tests flip this to exercise the cooperative drain

  void requestStop() override {
    ++stopCalls;
    if (log) *log += "cap.reqStop ";
  }

  bool isDrained() const override { return drained; }

  void finishStop() override {
    ++finishCalls;
    if (log) *log += "cap.finStop ";
  }

  uint32_t maxReadMicros() const override { return fakeMaxReadMicros; }
  void resetReadStats() override { fakeMaxReadMicros = 0; }
};

struct Harness {
  FakeAudioDevice device;
  tth::AudioBus bus;
  FakeCapture capture;
  tth::CaptureController controller;

  Harness()
      : bus(device), controller(bus, capture, kMaxDurationMs, kDrainTimeoutMs) {
    capture.log = &device.log;
    memset(g_turnStorage, 0, sizeof(g_turnStorage));
    memset(g_chunkStorage, 0, sizeof(g_chunkStorage));
    controller.begin(g_turnStorage, kCapacity, g_chunkStorage, kChunk);
  }
};

}  // namespace

void setUp() {}
void tearDown() {}

// --- the normal turn --------------------------------------------------------

static void a_normal_turn_starts_records_and_stops() {
  Harness h;

  TEST_ASSERT_TRUE(h.controller.start(1000));
  TEST_ASSERT_TRUE(h.controller.isCapturing());
  TEST_ASSERT_TRUE(h.bus.micOwns());

  for (uint32_t t = 1000; t < 1300; t += 32) h.controller.poll(t);

  TEST_ASSERT_TRUE(h.controller.isCapturing());
  TEST_ASSERT_TRUE(h.controller.sampleCount() > 0);

  h.controller.stop(1500, tth::CaptureStopReason::ButtonRelease);

  TEST_ASSERT_FALSE(h.controller.isCapturing());
  TEST_ASSERT_EQUAL(tth::CaptureStopReason::ButtonRelease,
                    h.controller.stopReason());
  TEST_ASSERT_EQUAL(tth::AudioOwner::None, h.bus.owner());
  TEST_ASSERT_EQUAL_UINT32(500, h.controller.metrics().durationMs);
}

// The ordering is the whole point: the bus must own the microphone before
// anything reads from it, and the reader must stop before the bus is released.
static void the_bus_is_acquired_before_capture_and_released_after() {
  Harness h;

  h.controller.start(0);
  TEST_ASSERT_EQUAL_STRING("bus.mic+ cap.start+ ", h.device.log.c_str());

  h.controller.stop(100, tth::CaptureStopReason::ButtonRelease);
  TEST_ASSERT_EQUAL_STRING("bus.mic+ cap.start+ cap.reqStop cap.finStop bus.mic- ",
                           h.device.log.c_str());
}

static void bytes_are_two_per_sample() {
  Harness h;
  h.controller.start(0);
  h.controller.poll(32);
  h.controller.stop(64, tth::CaptureStopReason::ButtonRelease);

  TEST_ASSERT_EQUAL_UINT32(h.controller.sampleCount() * 2u,
                           h.controller.byteCount());
  TEST_ASSERT_EQUAL_UINT32(h.controller.byteCount(),
                           h.controller.metrics().bytes);
}

// --- failures ---------------------------------------------------------------

// If the bus refuses, nothing is left owned and the caller must not enter
// LISTENING.
static void a_failed_microphone_acquisition_owns_nothing() {
  Harness h;
  h.device.micBeginResult = false;

  TEST_ASSERT_FALSE(h.controller.start(0));

  TEST_ASSERT_FALSE(h.controller.isCapturing());
  TEST_ASSERT_EQUAL(tth::CaptureStopReason::AcquireFailed,
                    h.controller.stopReason());
  TEST_ASSERT_EQUAL(tth::AudioOwner::None, h.bus.owner());
  TEST_ASSERT_EQUAL_INT(0, h.capture.startCalls);
}

// If the bus succeeds but the reader will not start, the bus must be handed
// back rather than left holding the microphone.
static void a_failed_capture_start_releases_the_bus() {
  Harness h;
  h.capture.startResult = false;

  TEST_ASSERT_FALSE(h.controller.start(0));

  TEST_ASSERT_FALSE(h.controller.isCapturing());
  TEST_ASSERT_EQUAL(tth::CaptureStopReason::AcquireFailed,
                    h.controller.stopReason());
  TEST_ASSERT_EQUAL(tth::AudioOwner::None, h.bus.owner());
  TEST_ASSERT_EQUAL_STRING("bus.mic+ cap.startx bus.mic- ",
                           h.device.log.c_str());
}

static void starting_without_buffers_fails_cleanly() {
  FakeAudioDevice device;
  tth::AudioBus bus(device);
  FakeCapture capture;
  tth::CaptureController controller(bus, capture, kMaxDurationMs,
                                    kDrainTimeoutMs);
  // begin() deliberately not called.

  TEST_ASSERT_FALSE(controller.start(0));
  TEST_ASSERT_EQUAL(tth::CaptureStopReason::AcquireFailed,
                    controller.stopReason());
  TEST_ASSERT_EQUAL(tth::AudioOwner::None, bus.owner());
  TEST_ASSERT_EQUAL_STRING("", device.log.c_str());
}

// A read failure mid-turn must end the turn and release everything.
static void a_failed_read_stops_the_turn_and_releases_everything() {
  Harness h;
  h.capture.failAfterReads = 3;

  h.controller.start(0);
  for (uint32_t t = 0; t < 1000 && h.controller.isCapturing(); t += 32) {
    h.controller.poll(t);
  }

  TEST_ASSERT_FALSE(h.controller.isCapturing());
  TEST_ASSERT_EQUAL(tth::CaptureStopReason::AudioError,
                    h.controller.stopReason());
  TEST_ASSERT_EQUAL(tth::AudioOwner::None, h.bus.owner());
  TEST_ASSERT_EQUAL_UINT32(1, h.controller.metrics().failedReads);
  // The three good chunks before the failure are kept.
  TEST_ASSERT_EQUAL_UINT32(3 * kChunk, h.controller.sampleCount());
}

// After an error the controller must be usable again.
static void a_turn_can_start_again_after_an_error() {
  Harness h;
  h.capture.failAfterReads = 1;

  h.controller.start(0);
  for (uint32_t t = 0; t < 500 && h.controller.isCapturing(); t += 32) {
    h.controller.poll(t);
  }
  TEST_ASSERT_EQUAL(tth::CaptureStopReason::AudioError,
                    h.controller.stopReason());

  h.capture.failAfterReads = -1;
  TEST_ASSERT_TRUE(h.controller.start(1000));
  TEST_ASSERT_TRUE(h.controller.isCapturing());
  TEST_ASSERT_EQUAL(tth::CaptureStopReason::None, h.controller.stopReason());
  TEST_ASSERT_EQUAL(tth::AudioOwner::Mic, h.bus.owner());
}

// --- capacity ---------------------------------------------------------------

// Filling the buffer must stop the turn cleanly, not overflow it.
static void a_full_buffer_forces_a_controlled_stop() {
  Harness h;
  h.controller.start(0);

  for (uint32_t t = 0; t < 100000 && h.controller.isCapturing(); t += 32) {
    h.controller.poll(t);
  }

  TEST_ASSERT_FALSE(h.controller.isCapturing());
  TEST_ASSERT_EQUAL(tth::CaptureStopReason::BufferFull,
                    h.controller.stopReason());
  TEST_ASSERT_EQUAL_UINT32(kCapacity, h.controller.sampleCount());
  TEST_ASSERT_EQUAL(tth::AudioOwner::None, h.bus.owner());
}

// kCapacity is an exact multiple of kChunk, so the last chunk lands precisely
// on the boundary: nothing may be dropped.
static void an_exact_boundary_drops_nothing() {
  Harness h;
  TEST_ASSERT_EQUAL_UINT32(0, kCapacity % kChunk);

  h.controller.start(0);
  for (uint32_t t = 0; t < 100000 && h.controller.isCapturing(); t += 32) {
    h.controller.poll(t);
  }

  TEST_ASSERT_EQUAL_UINT32(kCapacity, h.controller.sampleCount());
  TEST_ASSERT_EQUAL_UINT32(0, h.controller.metrics().droppedSamples);
}

// A chunk that does not divide the capacity evenly must have its overflow
// counted, never silently discarded.
static void an_overflowing_chunk_counts_what_did_not_fit() {
  Harness h;
  h.capture.samplesPerRead = 48;  // 1024 is not a multiple of 48

  h.controller.start(0);
  for (uint32_t t = 0; t < 100000 && h.controller.isCapturing(); t += 32) {
    h.controller.poll(t);
  }

  TEST_ASSERT_EQUAL(tth::CaptureStopReason::BufferFull,
                    h.controller.stopReason());
  TEST_ASSERT_EQUAL_UINT32(kCapacity, h.controller.sampleCount());
  TEST_ASSERT_TRUE(h.controller.metrics().droppedSamples > 0);
  // 21 chunks of 48 fill 1008; the 22nd offers 48 but only 16 fit, so 32
  // samples did not -- counted, not silently discarded.
  TEST_ASSERT_EQUAL_UINT32(32, h.controller.metrics().droppedSamples);
}

// --- the maximum hold -------------------------------------------------------

// The controller enforces the ceiling itself, independently of the button.
static void the_maximum_hold_stops_the_turn() {
  // A capacity large enough that the ceiling, not the buffer, ends the turn.
  static int16_t bigStorage[4000];
  FakeAudioDevice device;
  tth::AudioBus bus(device);
  FakeCapture capture;
  capture.samplesPerRead = 0;  // no data, so the buffer never fills
  tth::CaptureController controller(bus, capture, kMaxDurationMs,
                                    kDrainTimeoutMs);
  controller.begin(bigStorage, 4000, g_chunkStorage, kChunk);

  controller.start(1000);
  controller.poll(1000 + kMaxDurationMs - 1);
  TEST_ASSERT_TRUE(controller.isCapturing());

  controller.poll(1000 + kMaxDurationMs);
  TEST_ASSERT_FALSE(controller.isCapturing());
  TEST_ASSERT_EQUAL(tth::CaptureStopReason::MaxHold, controller.stopReason());
  TEST_ASSERT_EQUAL(tth::AudioOwner::None, bus.owner());
}

// --- stop discipline --------------------------------------------------------

// Nothing may be recorded after the turn has ended.
static void polling_after_a_stop_records_nothing() {
  Harness h;
  h.controller.start(0);
  h.controller.poll(32);
  const uint32_t afterOneChunk = h.controller.sampleCount();
  TEST_ASSERT_TRUE(afterOneChunk > 0);

  h.controller.stop(64, tth::CaptureStopReason::ButtonRelease);
  const int readsAtStop = h.capture.reads;

  for (uint32_t t = 100; t < 2000; t += 32) h.controller.poll(t);

  TEST_ASSERT_EQUAL_UINT32(afterOneChunk, h.controller.sampleCount());
  TEST_ASSERT_EQUAL_INT(readsAtStop, h.capture.reads);
}

// stop() must be safe to call again -- App calls it on the button release even
// if the controller already stopped itself.
static void stopping_twice_is_harmless() {
  Harness h;
  h.controller.start(0);
  h.controller.stop(100, tth::CaptureStopReason::ButtonRelease);
  const std::string afterFirst = h.device.log;

  h.controller.stop(200, tth::CaptureStopReason::MaxHold);

  // No extra hardware calls, and the original reason stands.
  TEST_ASSERT_EQUAL_STRING(afterFirst.c_str(), h.device.log.c_str());
  TEST_ASSERT_EQUAL(tth::CaptureStopReason::ButtonRelease,
                    h.controller.stopReason());
  TEST_ASSERT_EQUAL_INT(1, h.capture.stopCalls);
}

static void starting_twice_does_not_reacquire() {
  Harness h;
  h.controller.start(0);
  const std::string afterFirst = h.device.log;

  TEST_ASSERT_TRUE(h.controller.start(50));
  TEST_ASSERT_EQUAL_STRING(afterFirst.c_str(), h.device.log.c_str());
  TEST_ASSERT_EQUAL_INT(1, h.capture.startCalls);
}

// --- repeated turns ---------------------------------------------------------

static void repeated_turns_each_start_clean() {
  Harness h;

  for (int turn = 0; turn < 5; ++turn) {
    const uint32_t base = static_cast<uint32_t>(turn) * 10000u;
    TEST_ASSERT_TRUE(h.controller.start(base));
    for (uint32_t t = base; t < base + 200; t += 32) h.controller.poll(t);
    h.controller.stop(base + 200, tth::CaptureStopReason::ButtonRelease);

    // Each turn holds only its own audio.
    TEST_ASSERT_EQUAL_UINT32(7 * kChunk, h.controller.sampleCount());
    TEST_ASSERT_EQUAL_UINT32(200, h.controller.metrics().durationMs);
    TEST_ASSERT_EQUAL(tth::AudioOwner::None, h.bus.owner());
  }

  // ...but the session figure spans every turn.
  TEST_ASSERT_EQUAL_UINT32(7 * kChunk * 2u,
                           h.controller.sessionHighWaterBytes());
}

// Metrics from a previous turn must never leak into the next one.
static void metrics_reset_between_turns() {
  Harness h;

  h.capture.failAfterReads = 2;
  h.controller.start(0);
  for (uint32_t t = 0; t < 500 && h.controller.isCapturing(); t += 32) {
    h.controller.poll(t);
  }
  TEST_ASSERT_EQUAL_UINT32(1, h.controller.metrics().failedReads);
  TEST_ASSERT_TRUE(h.controller.metrics().chunks > 0);

  h.capture.failAfterReads = -1;
  h.capture.reads = 0;
  h.controller.start(1000);

  const tth::CaptureMetrics& m = h.controller.metrics();
  TEST_ASSERT_EQUAL_UINT32(0, m.failedReads);
  TEST_ASSERT_EQUAL_UINT32(0, m.chunks);
  TEST_ASSERT_EQUAL_UINT32(0, m.samples);
  TEST_ASSERT_EQUAL_UINT32(0, m.bytes);
  TEST_ASSERT_EQUAL_UINT32(0, m.droppedSamples);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, m.absolutePeak);
  TEST_ASSERT_EQUAL_UINT32(0, h.controller.sampleCount());
}

// --- metrics ----------------------------------------------------------------

static void levels_are_measured_across_the_turn() {
  Harness h;
  h.capture.sampleValue = 0;  // silence: DC removal leaves it at zero

  h.controller.start(0);
  h.controller.poll(32);
  h.controller.poll(64);
  h.controller.stop(96, tth::CaptureStopReason::ButtonRelease);

  const tth::CaptureMetrics& m = h.controller.metrics();
  TEST_ASSERT_EQUAL_UINT32(2, m.chunks);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, m.averageRms);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, m.absolutePeak);
  // A silent turn must report a minimum of 0, not the 1.0 sentinel.
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, m.minRms);
}

// A turn with no chunks at all must not report the sentinel minimum.
static void a_turn_with_no_audio_reports_zero_minimum() {
  Harness h;
  h.capture.samplesPerRead = 0;

  h.controller.start(0);
  h.controller.poll(32);
  h.controller.stop(64, tth::CaptureStopReason::ButtonRelease);

  TEST_ASSERT_EQUAL_UINT32(0, h.controller.metrics().chunks);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, h.controller.metrics().minRms);
}

static void stop_reason_names_are_stable() {
  TEST_ASSERT_EQUAL_STRING("none", tth::toString(tth::CaptureStopReason::None));
  TEST_ASSERT_EQUAL_STRING(
      "button release", tth::toString(tth::CaptureStopReason::ButtonRelease));
  TEST_ASSERT_EQUAL_STRING("maximum hold",
                           tth::toString(tth::CaptureStopReason::MaxHold));
  TEST_ASSERT_EQUAL_STRING("buffer full",
                           tth::toString(tth::CaptureStopReason::BufferFull));
  TEST_ASSERT_EQUAL_STRING("audio error",
                           tth::toString(tth::CaptureStopReason::AudioError));
  TEST_ASSERT_EQUAL_STRING(
      "microphone unavailable",
      tth::toString(tth::CaptureStopReason::AcquireFailed));
}

// --- cooperative draining ------------------------------------------------

// stop() must not wait for the hardware. When the microphone is still busy the
// controller enters Draining, keeps the bus, and finishes across later polls.
static void stopping_while_busy_defers_the_release() {
  Harness h;
  h.controller.start(0);
  h.capture.drained = false;

  h.controller.stop(100, tth::CaptureStopReason::ButtonRelease);

  TEST_ASSERT_FALSE(h.controller.isCapturing());
  TEST_ASSERT_TRUE(h.controller.isDraining());
  TEST_ASSERT_TRUE(h.controller.isBusy());
  // The microphone is still OURS until the task has finished with the buffers.
  TEST_ASSERT_EQUAL(tth::AudioOwner::Mic, h.bus.owner());
  TEST_ASSERT_EQUAL_STRING("bus.mic+ cap.start+ cap.reqStop ",
                           h.device.log.c_str());

  // Still busy a few polls later: nothing released, nothing blocked.
  h.controller.poll(110);
  h.controller.poll(120);
  TEST_ASSERT_TRUE(h.controller.isDraining());
  TEST_ASSERT_EQUAL(tth::AudioOwner::Mic, h.bus.owner());

  h.capture.drained = true;
  h.controller.poll(130);

  TEST_ASSERT_FALSE(h.controller.isBusy());
  TEST_ASSERT_EQUAL(tth::AudioOwner::None, h.bus.owner());
  TEST_ASSERT_EQUAL_STRING(
      "bus.mic+ cap.start+ cap.reqStop cap.finStop bus.mic- ",
      h.device.log.c_str());
}

// A drain that never completes must not hold the microphone forever.
static void a_stuck_drain_gives_up_at_the_deadline() {
  Harness h;
  h.controller.start(0);
  h.capture.drained = false;
  h.controller.stop(100, tth::CaptureStopReason::ButtonRelease);

  h.controller.poll(100 + kDrainTimeoutMs - 1);
  TEST_ASSERT_TRUE(h.controller.isDraining());

  h.controller.poll(100 + kDrainTimeoutMs);
  TEST_ASSERT_FALSE(h.controller.isBusy());
  TEST_ASSERT_EQUAL(tth::AudioOwner::None, h.bus.owner());
  TEST_ASSERT_EQUAL_UINT32(1, h.controller.drainTimeouts());
}

// A new turn must not start while the previous one is still handing the
// microphone back.
static void a_turn_cannot_start_while_draining() {
  Harness h;
  h.controller.start(0);
  h.capture.drained = false;
  h.controller.stop(100, tth::CaptureStopReason::ButtonRelease);

  TEST_ASSERT_FALSE(h.controller.start(110));
  TEST_ASSERT_TRUE(h.controller.isDraining());

  h.capture.drained = true;
  h.controller.poll(120);
  TEST_ASSERT_TRUE(h.controller.start(130));
}

// --- per-turn metric reset ------------------------------------------------

// The reported defect: a short second turn showed the FIRST turn's high-water
// mark in its summary.
static void the_high_water_mark_is_per_turn_not_carried_over() {
  Harness h;

  // First turn: long. 10 polls x 64 samples = 640.
  h.controller.start(0);
  for (uint32_t t = 0; t < 320; t += 32) h.controller.poll(t);
  h.controller.stop(320, tth::CaptureStopReason::ButtonRelease);
  const uint32_t firstSamples = h.controller.sampleCount();
  TEST_ASSERT_EQUAL_UINT32(firstSamples, h.controller.highWaterSamples());
  TEST_ASSERT_TRUE(firstSamples >= 640);

  // Second turn: deliberately shorter. 3 polls x 64 = 192.
  h.controller.start(1000);
  for (uint32_t t = 1000; t < 1096; t += 32) h.controller.poll(t);
  h.controller.stop(1096, tth::CaptureStopReason::ButtonRelease);
  const uint32_t secondSamples = h.controller.sampleCount();

  TEST_ASSERT_TRUE(secondSamples < firstSamples);
  // The summary figure must describe THIS turn.
  TEST_ASSERT_EQUAL_UINT32(secondSamples, h.controller.highWaterSamples());
  TEST_ASSERT_EQUAL_UINT32(secondSamples * 2u, h.controller.highWaterBytes());

  // ...while the session figure still remembers the longest turn, which is
  // what judges whether the capacity is right.
  TEST_ASSERT_EQUAL_UINT32(firstSamples * 2u,
                           h.controller.sessionHighWaterBytes());
}

// Every other per-turn figure must reset too.
static void every_per_turn_metric_resets_on_start() {
  Harness h;

  h.capture.alternate = true;  // real signal, so peak is non-zero
  h.capture.failAfterReads = 2;
  h.controller.start(0);
  for (uint32_t t = 0; t < 500 && h.controller.isCapturing(); t += 32) {
    h.controller.poll(t);
  }
  TEST_ASSERT_TRUE(h.controller.metrics().failedReads > 0);
  TEST_ASSERT_TRUE(h.controller.metrics().chunks > 0);
  TEST_ASSERT_TRUE(h.controller.metrics().absolutePeak > 0.0f);

  h.capture.failAfterReads = -1;
  h.capture.reads = 0;
  h.controller.start(1000);

  const tth::CaptureMetrics& m = h.controller.metrics();
  TEST_ASSERT_EQUAL_UINT32(0, m.durationMs);
  TEST_ASSERT_EQUAL_UINT32(0, m.samples);
  TEST_ASSERT_EQUAL_UINT32(0, m.bytes);
  TEST_ASSERT_EQUAL_UINT32(0, m.chunks);
  TEST_ASSERT_EQUAL_UINT32(0, m.failedReads);
  TEST_ASSERT_EQUAL_UINT32(0, m.droppedSamples);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, m.rms);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, m.peak);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, m.maxRms);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, m.averageRms);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, m.absolutePeak);
  TEST_ASSERT_EQUAL_UINT32(0, h.controller.sampleCount());
  TEST_ASSERT_EQUAL_UINT32(0, h.controller.highWaterSamples());
  TEST_ASSERT_EQUAL(tth::CaptureStopReason::None, h.controller.stopReason());
  TEST_ASSERT_EQUAL_UINT32(0, h.capture.fakeMaxReadMicros);
}

static void capture_state_names_are_stable() {
  TEST_ASSERT_EQUAL_STRING("idle", tth::toString(tth::CaptureState::Idle));
  TEST_ASSERT_EQUAL_STRING("recording",
                           tth::toString(tth::CaptureState::Recording));
  TEST_ASSERT_EQUAL_STRING("draining",
                           tth::toString(tth::CaptureState::Draining));
}

// --- Phase 5: TurnBuffer lifetime ----------------------------------------------

// A reader (the streamer, or loopback playback) still holds the previous turn.
// The next start must be refused BEFORE anything is touched: no hardware call,
// no reset of the audio still being read.
static void start_is_refused_while_a_reader_holds_the_buffer() {
  Harness h;
  TEST_ASSERT_TRUE(h.controller.start(1000));
  for (uint32_t t = 1000; t < 1200; t += 32) h.controller.poll(t);
  h.controller.stop(1300, tth::CaptureStopReason::ButtonRelease);
  const uint32_t recorded = h.controller.sampleCount();
  TEST_ASSERT_TRUE(recorded > 0);
  TEST_ASSERT_EQUAL(tth::AudioOwner::None, h.bus.owner());

  TEST_ASSERT_TRUE(h.controller.turn().retain());
  h.device.log.clear();

  TEST_ASSERT_FALSE(h.controller.start(2000));
  TEST_ASSERT_EQUAL(tth::CaptureStopReason::BufferInUse,
                    h.controller.stopReason());
  TEST_ASSERT_EQUAL_STRING("", h.device.log.c_str());
  TEST_ASSERT_EQUAL_UINT32(recorded, h.controller.sampleCount());
  TEST_ASSERT_EQUAL_UINT32(recorded, h.controller.turn().committedSamples());
  TEST_ASSERT_EQUAL(tth::AudioOwner::None, h.bus.owner());

  h.controller.turn().release();
  TEST_ASSERT_TRUE(h.controller.start(3000));
  TEST_ASSERT_EQUAL_UINT32(0, h.controller.sampleCount());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(start_is_refused_while_a_reader_holds_the_buffer);
  RUN_TEST(stopping_while_busy_defers_the_release);
  RUN_TEST(a_stuck_drain_gives_up_at_the_deadline);
  RUN_TEST(a_turn_cannot_start_while_draining);
  RUN_TEST(the_high_water_mark_is_per_turn_not_carried_over);
  RUN_TEST(every_per_turn_metric_resets_on_start);
  RUN_TEST(capture_state_names_are_stable);
  RUN_TEST(a_normal_turn_starts_records_and_stops);
  RUN_TEST(the_bus_is_acquired_before_capture_and_released_after);
  RUN_TEST(bytes_are_two_per_sample);
  RUN_TEST(a_failed_microphone_acquisition_owns_nothing);
  RUN_TEST(a_failed_capture_start_releases_the_bus);
  RUN_TEST(starting_without_buffers_fails_cleanly);
  RUN_TEST(a_failed_read_stops_the_turn_and_releases_everything);
  RUN_TEST(a_turn_can_start_again_after_an_error);
  RUN_TEST(a_full_buffer_forces_a_controlled_stop);
  RUN_TEST(an_exact_boundary_drops_nothing);
  RUN_TEST(an_overflowing_chunk_counts_what_did_not_fit);
  RUN_TEST(the_maximum_hold_stops_the_turn);
  RUN_TEST(polling_after_a_stop_records_nothing);
  RUN_TEST(stopping_twice_is_harmless);
  RUN_TEST(starting_twice_does_not_reacquire);
  RUN_TEST(repeated_turns_each_start_clean);
  RUN_TEST(metrics_reset_between_turns);
  RUN_TEST(levels_are_measured_across_the_turn);
  RUN_TEST(a_turn_with_no_audio_reports_zero_minimum);
  RUN_TEST(stop_reason_names_are_stable);
  return UNITY_END();
}
