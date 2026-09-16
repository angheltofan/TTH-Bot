#pragma once

#include <stdint.h>

#include "tth/HapticPattern.h"

namespace tth {

// The Core2's vibration motor (AXP192 LDO3, via M5.Power.setVibration).
//
// ONE use only: the two-short-pulse pattern when an action is refused
// (push-to-talk while not online, the activity menu while busy or offline).
// There is deliberately no vibration when the robot starts speaking.
//
// Non-blocking: denied() switches the motor on and returns; poll() switches
// it off and on as the pattern's durations pass. No delay() anywhere. The
// timing itself is portable and host-tested (HapticPattern).
class Haptics {
 public:
  Haptics();

  // Two short pulses: an action was refused. Returns false if the pattern is
  // already running.
  bool denied(uint32_t nowMs);

  // Once per loop.
  void poll(uint32_t nowMs);

  // Refused patterns started (the heartbeat's hapticsRefused).
  uint32_t deniedCount() const { return _denied.starts(); }
  bool isActive() const { return _denied.isActive(); }

 private:
  HapticPattern _denied;
};

}  // namespace tth
