#include "tth/HapticPattern.h"

namespace tth {

HapticPattern::HapticPattern(const uint16_t* stepsMs, uint8_t count)
    : _count(0),
      _active(false),
      _motorOn(false),
      _index(0),
      _stepStartMs(0),
      _starts(0) {
  for (uint8_t i = 0; i < kMaxSteps; ++i) _steps[i] = 0;
  if (stepsMs == nullptr) return;
  _count = count;
  if (_count > kMaxSteps) _count = kMaxSteps;
  for (uint8_t i = 0; i < _count; ++i) _steps[i] = stepsMs[i];
}

bool HapticPattern::start(uint32_t nowMs) {
  if (_active || _count == 0) return false;
  _active = true;
  _motorOn = true;
  _index = 0;
  _stepStartMs = nowMs;
  ++_starts;
  return true;
}

HapticPattern::Motor HapticPattern::poll(uint32_t nowMs) {
  if (!_active) return Motor::NoChange;
  while (_index < _count && (nowMs - _stepStartMs) >= _steps[_index]) {
    _stepStartMs += _steps[_index];
    ++_index;
  }
  const bool wantOn = (_index < _count) && ((_index % 2u) == 0u);
  if (_index >= _count) _active = false;
  if (wantOn == _motorOn) return Motor::NoChange;
  _motorOn = wantOn;
  return wantOn ? Motor::On : Motor::Off;
}

}  // namespace tth
