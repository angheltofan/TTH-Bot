#pragma once

#include <stdint.h>

#include "tth/AudioBus.h"
#include "tth/IAudioCapture.h"
#include "tth/StageTimer.h"
#include "tth/TurnBuffer.h"

// Owns one push-to-talk recording, start to finish.
//
// It is the only thing that acquires the microphone for capture, and it always
// releases it again -- on a normal stop, on a failure to start, and on a read
// error. That single-owner rule is what stops the microphone being left
// installed after something goes wrong, which on this board would block the
// speaker from ever starting.
//
// ORDERING, which the tests pin:
//   start : AudioBus::acquireMic()  THEN  IAudioCapture::startCapture()
//   stop  : IAudioCapture::stopCapture()  THEN  AudioBus::releaseAll()
//
// Portable and clock-injected: no M5Unified, no timer of its own. The 45
// second ceiling is therefore tested by passing 45000, not by waiting.

namespace tth {

enum class CaptureStopReason : uint8_t {
  // Never started, or still running.
  None = 0,
  // The child let go of the button. The normal case.
  ButtonRelease,
  // The maximum hold was reached while still held.
  MaxHold,
  // The preallocated buffer filled up. Defensive: the buffer is sized past
  // the maximum hold, so this should be unreachable in practice.
  BufferFull,
  // The microphone reported a read failure mid-turn.
  AudioError,
  // The microphone could not be acquired or started at all.
  AcquireFailed,
  // A reader (the streamer, or loopback playback) still holds the turn
  // buffer. Starting would overwrite audio that has not been consumed, so the
  // start is refused and nothing is touched.
  BufferInUse,
  // The turn failed elsewhere (Step 6.3: the gateway session ended, the
  // gateway reported an error, or a protocol check failed). The recording is
  // abandoned; nothing more is sent.
  TurnFailed,
};

const char* toString(CaptureStopReason reason);

struct CaptureMetrics {
  uint32_t durationMs;
  uint32_t samples;
  uint32_t bytes;

  // Levels of the most recent chunk, for the live status line.
  float rms;
  float peak;

  // Whole-turn statistics.
  float minRms;
  float maxRms;
  float averageRms;
  float absolutePeak;

  uint32_t chunks;
  uint32_t failedReads;
  // Samples the microphone produced that did not fit in the buffer. Counted
  // rather than silently dropped.
  uint32_t droppedSamples;
};

enum class CaptureState : uint8_t {
  // Nothing owned, nothing running.
  Idle = 0,
  // Recording into the turn buffer.
  Recording,
  // The turn is over and the UI has already moved on, but the microphone task
  // may still be writing our buffers. Polled across loop iterations; the bus
  // is NOT released until this completes. Waiting for it synchronously is what
  // pushed a loop iteration to 90 ms.
  Draining,
};

const char* toString(CaptureState state);

class CaptureController {
 public:
  CaptureController(AudioBus& bus, IAudioCapture& capture,
                    uint32_t maxDurationMs, uint32_t drainTimeoutMs);

  // Hands over the preallocated PSRAM storage and the scratch chunk the
  // microphone reads into. Called once, at start-up.
  void begin(int16_t* turnStorage, uint32_t turnCapacitySamples,
             int16_t* chunkStorage, uint32_t chunkCapacitySamples);

  // Optional. When set, each stage inside start() is measured and reported,
  // so a slow start can be attributed rather than guessed at.
  void setStageTimer(IStageTimer* timer) { _stageTimer = timer; }

  // Acquires the microphone and starts recording. Returns false if either
  // step fails, in which case nothing is left owned and stopReason() explains
  // why. The caller must not enter LISTENING when this returns false.
  bool start(uint32_t nowMs);

  // Services the capture. Non-blocking; may stop the turn by itself on a read
  // failure, a full buffer or the maximum hold. Callers detect that by
  // watching isCapturing().
  void poll(uint32_t nowMs);

  // Ends the turn. Idempotent, so it is safe to call after the controller has
  // already stopped itself.
  void stop(uint32_t nowMs, CaptureStopReason reason);

  bool isCapturing() const { return _state == CaptureState::Recording; }
  bool isDraining() const { return _state == CaptureState::Draining; }
  // True while the microphone is still owned in any form. The next turn must
  // not start until this is false.
  bool isBusy() const { return _state != CaptureState::Idle; }
  CaptureState state() const { return _state; }

  // Elapsed time from the stop request until the hardware was actually
  // released. Around 60 ms is expected: two queued 32 ms chunks must finish.
  // This is WALL TIME spread across loop iterations, not time spent blocked.
  uint32_t lastDrainTotalMs() const { return _lastDrainTotalMs; }
  uint32_t maxDrainTotalMs() const { return _maxDrainTotalMs; }
  uint32_t drainTimeouts() const { return _drainTimeouts; }
  CaptureStopReason stopReason() const { return _stopReason; }
  const CaptureMetrics& metrics() const { return _metrics; }

  // The turn buffer itself, for its readers (TurnStreamer, loopback playback).
  // They read committed samples only and hold a lease while they do.
  TurnBuffer& turn() { return _turn; }

  // The captured PCM: mono, signed 16-bit little-endian, 16 kHz.
  const int16_t* data() const { return _turn.data(); }
  uint32_t sampleCount() const { return _turn.size(); }
  uint32_t byteCount() const { return _turn.byteSize(); }
  uint32_t capacitySamples() const { return _turn.capacity(); }
  uint32_t capacityBytes() const { return _turn.byteCapacity(); }
  // Per-turn: reset by start(), so a summary never reports a previous turn.
  uint32_t highWaterSamples() const { return _turn.highWaterSamples(); }
  uint32_t highWaterBytes() const { return _turn.highWaterSamples() * 2u; }
  // Whole session: for judging whether the capacity is right.
  uint32_t sessionHighWaterBytes() const {
    return _turn.sessionHighWaterSamples() * 2u;
  }

 private:
  void resetMetrics();
  void accumulate(uint32_t storedSamples, float rms, float peak);
  void serviceDrain(uint32_t nowMs);

  AudioBus& _bus;
  IAudioCapture& _capture;
  const uint32_t _maxDurationMs;

  TurnBuffer _turn;
  int16_t* _chunk;
  uint32_t _chunkCapacity;
  IStageTimer* _stageTimer;

  CaptureState _state;
  uint32_t _startedMs;
  uint32_t _drainStartedMs;
  const uint32_t _drainTimeoutMs;
  uint32_t _lastDrainTotalMs;
  uint32_t _maxDrainTotalMs;
  uint32_t _drainTimeouts;
  CaptureStopReason _stopReason;
  CaptureMetrics _metrics;
  double _rmsSum;
};

}  // namespace tth
