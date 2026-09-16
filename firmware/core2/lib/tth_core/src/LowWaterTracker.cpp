#include "tth/LowWaterTracker.h"

namespace tth {

LowWaterTracker::LowWaterTracker(uint32_t reportStepBytes)
    : _step(reportStepBytes == 0 ? 1u : reportStepBytes),
      _started(false),
      _low(0),
      _reported(0),
      _lastDrop(0),
      _stage(""),
      _reports(0),
      _decreases(0) {}

void LowWaterTracker::reset(uint32_t lowWater) {
  _started = true;
  _low = lowWater;
  _reported = lowWater;
  _lastDrop = 0;
  _stage = "";
}

bool LowWaterTracker::observe(uint32_t lowWater, const char* stage) {
  if (!_started) {
    reset(lowWater);
    return false;
  }
  if (lowWater >= _low) return false;
  ++_decreases;
  _low = lowWater;
  if (_reported - lowWater < _step) return false;
  _lastDrop = _reported - lowWater;
  _reported = lowWater;
  _stage = (stage != nullptr) ? stage : "";
  ++_reports;
  return true;
}

}  // namespace tth
