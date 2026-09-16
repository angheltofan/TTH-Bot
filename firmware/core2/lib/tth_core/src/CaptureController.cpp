#include "tth/CaptureController.h"

#include "tth/PcmProcessing.h"

namespace tth {

const char* toString(CaptureStopReason reason) {
  switch (reason) {
    case CaptureStopReason::None:
      return "none";
    case CaptureStopReason::ButtonRelease:
      return "button release";
    case CaptureStopReason::MaxHold:
      return "maximum hold";
    case CaptureStopReason::BufferFull:
      return "buffer full";
    case CaptureStopReason::AudioError:
      return "audio error";
    case CaptureStopReason::AcquireFailed:
      return "microphone unavailable";
    case CaptureStopReason::BufferInUse:
      return "buffer in use";
    case CaptureStopReason::TurnFailed:
      return "turn failed";
  }
  return "invalid";
}

const char* toString(CaptureState state) {
  switch (state) {
    case CaptureState::Idle:
      return "idle";
    case CaptureState::Recording:
      return "recording";
    case CaptureState::Draining:
      return "draining";
  }
  return "invalid";
}

CaptureController::CaptureController(AudioBus& bus, IAudioCapture& capture,
                                     uint32_t maxDurationMs,
                                     uint32_t drainTimeoutMs)
    : _bus(bus),
      _capture(capture),
      _maxDurationMs(maxDurationMs),
      _chunk(nullptr),
      _chunkCapacity(0),
      _stageTimer(nullptr),
      _state(CaptureState::Idle),
      _startedMs(0),
      _drainStartedMs(0),
      _drainTimeoutMs(drainTimeoutMs),
      _lastDrainTotalMs(0),
      _maxDrainTotalMs(0),
      _drainTimeouts(0),
      _stopReason(CaptureStopReason::None),
      _rmsSum(0.0) {
  resetMetrics();
}

void CaptureController::begin(int16_t* turnStorage,
                              uint32_t turnCapacitySamples, int16_t* chunkStorage,
                              uint32_t chunkCapacitySamples) {
  _turn.attach(turnStorage, turnCapacitySamples);
  _chunk = chunkStorage;
  _chunkCapacity = (chunkStorage == nullptr) ? 0 : chunkCapacitySamples;
}

void CaptureController::resetMetrics() {
  _metrics.durationMs = 0;
  _metrics.samples = 0;
  _metrics.bytes = 0;
  _metrics.rms = 0.0f;
  _metrics.peak = 0.0f;
  // Starts above the maximum so the first chunk always replaces it.
  _metrics.minRms = 1.0f;
  _metrics.maxRms = 0.0f;
  _metrics.averageRms = 0.0f;
  _metrics.absolutePeak = 0.0f;
  _metrics.chunks = 0;
  _metrics.failedReads = 0;
  _metrics.droppedSamples = 0;
  _rmsSum = 0.0;
}

bool CaptureController::start(uint32_t nowMs) {
  if (_state == CaptureState::Recording) return true;
  // A previous turn is still handing the microphone back. Refusing here is
  // what stops two turns owning the hardware at once.
  if (_state == CaptureState::Draining) return false;

  // TurnBuffer lifetime invariant: a reader still holds the previous turn's
  // audio. Refuse BEFORE touching anything, so the refusal changes nothing --
  // not the buffer, not the metrics, not the bus.
  if (_turn.isRetained()) {
    _stopReason = CaptureStopReason::BufferInUse;
    return false;
  }

  // EVERY per-turn figure resets here. A summary must describe its own turn
  // and nothing else -- a stale high-water mark from a longer previous turn is
  // worse than no value at all, because it looks authoritative.
  //
  // Each stage below is measured when a stage timer is attached, so a slow
  // start is attributed to a named step rather than inferred.
  {
    ScopedStage stage(_stageTimer, "start.resetMetrics");
    resetMetrics();
  }
  {
    // Note this is O(1): it clears two counters, it does NOT wipe the 1.4 MB
    // buffer. Worth measuring precisely because that would be an easy thing to
    // get wrong later.
    ScopedStage stage(_stageTimer, "start.bufferReset");
    _turn.reset();
  }
  {
    ScopedStage stage(_stageTimer, "start.resetReadStats");
    _capture.resetReadStats();
  }

  _stopReason = CaptureStopReason::None;
  _startedMs = nowMs;
  _drainStartedMs = 0;

  if (!_turn.isAttached() || _chunk == nullptr) {
    _stopReason = CaptureStopReason::AcquireFailed;
    return false;
  }

  // The microphone must own the bus before anything tries to read from it.
  bool acquired = false;
  {
    ScopedStage stage(_stageTimer, "start.acquireMic");
    acquired = _bus.acquireMic();
  }
  if (!acquired) {
    _stopReason = CaptureStopReason::AcquireFailed;
    return false;
  }

  bool capturing = false;
  {
    ScopedStage stage(_stageTimer, "start.startCapture");
    capturing = _capture.startCapture();
  }
  if (!capturing) {
    _stopReason = CaptureStopReason::AcquireFailed;
    // Hand the bus back rather than leaving the microphone installed with
    // nothing reading it. Nothing has been queued, so there is nothing to
    // drain first.
    _bus.releaseAll();
    return false;
  }

  _state = CaptureState::Recording;
  return true;
}

void CaptureController::accumulate(uint32_t storedSamples, float rms,
                                   float peak) {
  _metrics.samples = _turn.size();
  _metrics.bytes = _turn.byteSize();
  _metrics.rms = rms;
  _metrics.peak = peak;

  if (rms < _metrics.minRms) _metrics.minRms = rms;
  if (rms > _metrics.maxRms) _metrics.maxRms = rms;
  if (peak > _metrics.absolutePeak) _metrics.absolutePeak = peak;

  _rmsSum += rms;
  ++_metrics.chunks;
  _metrics.averageRms =
      static_cast<float>(_rmsSum / static_cast<double>(_metrics.chunks));

  (void)storedSamples;
}

void CaptureController::serviceDrain(uint32_t nowMs) {
  const uint32_t waited = nowMs - _drainStartedMs;

  const bool drained = _capture.isDrained();
  const bool timedOut = waited >= _drainTimeoutMs;

  if (!drained && !timedOut) return;  // come back next loop

  if (!drained) ++_drainTimeouts;
  _lastDrainTotalMs = waited;
  if (waited > _maxDrainTotalMs) _maxDrainTotalMs = waited;

  _capture.finishStop();
  // ONLY NOW is it safe to tear the microphone down: the task has finished
  // with both buffers.
  _bus.releaseAll();
  _state = CaptureState::Idle;
}

void CaptureController::poll(uint32_t nowMs) {
  if (_state == CaptureState::Draining) {
    serviceDrain(nowMs);
    return;
  }
  if (_state != CaptureState::Recording) return;

  _metrics.durationMs = nowMs - _startedMs;

  const int read = _capture.readChunk(_chunk, _chunkCapacity);

  if (read < 0) {
    ++_metrics.failedReads;
    stop(nowMs, CaptureStopReason::AudioError);
    return;
  }

  if (read > 0) {
    const uint32_t count = static_cast<uint32_t>(read);
    // Centre the chunk and measure what will actually be stored.
    const ChunkLevels levels = removeDcOffsetAndMeasure(_chunk, count);

    const uint32_t stored = _turn.append(_chunk, count);
    if (stored < count) _metrics.droppedSamples += (count - stored);

    accumulate(stored, levels.rms, levels.peak);

    if (_turn.isFull()) {
      stop(nowMs, CaptureStopReason::BufferFull);
      return;
    }
  }

  // Defence in depth: the push-to-talk input enforces the same ceiling, but a
  // turn must not be able to outlive it even if that guard is bypassed.
  if (_metrics.durationMs >= _maxDurationMs) {
    stop(nowMs, CaptureStopReason::MaxHold);
  }
}

void CaptureController::stop(uint32_t nowMs, CaptureStopReason reason) {
  if (_state != CaptureState::Recording) return;

  _metrics.durationMs = nowMs - _startedMs;
  _metrics.samples = _turn.size();
  _metrics.bytes = _turn.byteSize();
  // A turn with no chunks would otherwise report a minimum of 1.0.
  if (_metrics.chunks == 0) _metrics.minRms = 0.0f;

  _stopReason = reason;

  // Ask the microphone to stop queueing, then hand over to the cooperative
  // drain. Nothing here waits: the caller's UI can move to WAITING at once
  // while the hardware finishes in the background.
  _capture.requestStop();
  _drainStartedMs = nowMs;
  _state = CaptureState::Draining;

  // If the hardware is already idle this completes immediately and the bus is
  // released in the same call -- the common case, costing nothing.
  serviceDrain(nowMs);
}

}  // namespace tth
