#pragma once

#include <stdint.h>

// A short non-blocking vibration pattern: alternating ON / OFF durations,
// starting with ON (PHASE6_PLAN §9: two short pulses when push-to-talk is
// refused because the robot is not online).
//
// Portable and clock-injected: it only says when the motor should change.

namespace tth {

class HapticPattern {
 public:
  static const uint8_t kMaxSteps = 8;

  enum class Motor : uint8_t { NoChange = 0, On, Off };

  HapticPattern(const uint16_t* stepsMs, uint8_t count);

  // True = switch the motor ON now. False if already running (not restarted).
  bool start(uint32_t nowMs);

  // The motor change due now, if any. A slow loop skips missed steps and
  // always ends with the motor off.
  Motor poll(uint32_t nowMs);

  bool isActive() const { return _active; }
  uint32_t starts() const { return _starts; }

 private:
  uint16_t _steps[kMaxSteps];
  uint8_t _count;
  bool _active;
  bool _motorOn;
  uint8_t _index;
  uint32_t _stepStartMs;
  uint32_t _starts;
};

}  // namespace tth
