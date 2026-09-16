#pragma once

#include <stdint.h>

#include "tth/HapticPattern.h"
#include "tth/HapticPulse.h"

namespace tth {

// The Core2's vibration motor (AXP192 LDO3, via M5.Power.setVibration).
//
// Non-blocking: pulse() and denied() switch the motor on and return; poll()
// switches it off (and back on, for a pattern) as the durations pass. No
// delay() anywhere. The timing itself is portable and host-tested
// (HapticPulse, HapticPattern). Only one of the two runs at a time.
class Haptics {
 public:
  Haptics();

  // One TTH_VIBRATION_MS pulse: the first audio of a response. Returns false
  // if a pulse or pattern is already running.
  bool pulse(uint32_t nowMs);

  // Two short pulses: push-to-talk refused because the robot is not online
  // (PHASE6_PLAN §9). Returns false if something is already running.
  bool denied(uint32_t nowMs);

  // Once per loop.
  void poll(uint32_t nowMs);

  uint32_t pulses() const { return _pulse.pulses(); }
  uint32_t deniedCount() const { return _denied.starts(); }
  bool isActive() const { return _pulse.isActive() || _denied.isActive(); }

 private:
  HapticPulse _pulse;
  HapticPattern _denied;
};

}  // namespace tth
