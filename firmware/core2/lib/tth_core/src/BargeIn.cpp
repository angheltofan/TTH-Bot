#include "tth/BargeIn.h"

namespace tth {

const char* toString(BargeInResult result) {
  switch (result) {
    case BargeInResult::Idle:
      return "idle";
    case BargeInResult::Pending:
      return "pending";
    case BargeInResult::Listening:
      return "listening";
    case BargeInResult::ReleasedEarly:
      return "released early";
    case BargeInResult::Failed:
      return "failed";
  }
  return "invalid";
}

BargeIn::BargeIn(IBargeInOps& ops)
    : _ops(ops),
      _timer(nullptr),
      _active(false),
      _startedMicros(0),
      _startedMs(0),
      _lastTotalMicros(0),
      _lastDrainMs(0),
      _count(0) {}

bool BargeIn::begin(uint32_t nowMs) {
  if (_active) return false;
  _active = true;
  ++_count;
  _startedMs = nowMs;
  _startedMicros = (_timer == nullptr) ? 0u : _timer->nowMicros();

  {
    ScopedStage stage(_timer, "barge.stopAccepting");
    _ops.stopAcceptingPlayback();
  }
  {
    ScopedStage stage(_timer, "barge.stopSpeaker");
    _ops.stopSpeaker(nowMs);
  }
  return true;
}

BargeInResult BargeIn::poll(uint32_t nowMs) {
  if (!_active) return BargeInResult::Idle;

  // Step 2 completes only when the speaker has let go of every buffer.
  if (!_ops.speakerQuiet()) return BargeInResult::Pending;
  _lastDrainMs = nowMs - _startedMs;

  {
    ScopedStage stage(_timer, "barge.cancelSource");
    _ops.cancelTurnSource();
  }
  {
    ScopedStage stage(_timer, "barge.releaseSpeaker");
    _ops.releaseSpeaker();
  }

  if (!_ops.stillHeld()) {
    finish(nowMs);
    return BargeInResult::ReleasedEarly;
  }

  bool started = false;
  {
    ScopedStage stage(_timer, "barge.startCapture");
    started = _ops.startCapture(nowMs);
  }
  finish(nowMs);
  return started ? BargeInResult::Listening : BargeInResult::Failed;
}

void BargeIn::finish(uint32_t nowMs) {
  (void)nowMs;
  _active = false;
  _lastTotalMicros =
      (_timer == nullptr) ? 0u : (_timer->nowMicros() - _startedMicros);
  if (_timer != nullptr) _timer->report("barge.total", _lastTotalMicros);
}

}  // namespace tth
