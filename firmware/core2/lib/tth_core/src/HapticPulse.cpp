#include "tth/HapticPulse.h"

namespace tth {

HapticPulse::HapticPulse(uint32_t durationMs)
    : _durationMs(durationMs), _active(false), _startedMs(0), _pulses(0) {}

bool HapticPulse::start(uint32_t nowMs) {
  if (_active) return false;
  _active = true;
  _startedMs = nowMs;
  ++_pulses;
  return true;
}

bool HapticPulse::poll(uint32_t nowMs) {
  if (!_active) return false;
  if ((nowMs - _startedMs) < _durationMs) return false;
  _active = false;
  return true;
}

}  // namespace tth
