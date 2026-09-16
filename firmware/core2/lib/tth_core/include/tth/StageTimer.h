#pragma once

#include <stdint.h>

// Lets portable code report how long its internal stages took, without
// depending on a clock or on Serial.
//
// CaptureController::start() measured ~33 ms and nothing inside it was
// individually instrumented, so the cost could only be inferred. This makes
// each stage measurable directly: the device supplies micros() and a place to
// put the numbers, and the portable logic stays portable.
//
// Optional everywhere -- a null timer means no measurement and no cost.

namespace tth {

class IStageTimer {
 public:
  virtual ~IStageTimer() {}
  virtual uint32_t nowMicros() const = 0;
  virtual void report(const char* stage, uint32_t elapsedMicros) = 0;
};

// Times one stage and reports it on destruction. Safe with a null timer.
class ScopedStage {
 public:
  ScopedStage(IStageTimer* timer, const char* stage)
      : _timer(timer),
        _stage(stage),
        _startMicros(timer == nullptr ? 0u : timer->nowMicros()) {}

  ~ScopedStage() {
    if (_timer == nullptr) return;
    _timer->report(_stage, _timer->nowMicros() - _startMicros);
  }

  ScopedStage(const ScopedStage&) = delete;
  ScopedStage& operator=(const ScopedStage&) = delete;

 private:
  IStageTimer* _timer;
  const char* _stage;
  const uint32_t _startMicros;
};

}  // namespace tth
