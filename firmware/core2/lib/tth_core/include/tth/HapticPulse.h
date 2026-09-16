#pragma once

#include <stdint.h>

// Timing for one non-blocking vibration pulse.
//
// Portable and clock-injected: it never touches the motor. The device layer
// switches the motor on when start() says to, and off when poll() says to, so
// the 120 ms duration and the "exactly one off" behaviour are tested on the
// host with no hardware and no waiting.

namespace tth {

class HapticPulse {
 public:
  explicit HapticPulse(uint32_t durationMs);

  // Returns true when the motor must be switched ON now. A pulse that is
  // already running is not restarted or extended (returns false).
  bool start(uint32_t nowMs);

  // Returns true exactly once per pulse: when the motor must be switched OFF.
  bool poll(uint32_t nowMs);

  bool isActive() const { return _active; }
  uint32_t pulses() const { return _pulses; }
  uint32_t durationMs() const { return _durationMs; }

 private:
  const uint32_t _durationMs;
  bool _active;
  uint32_t _startedMs;
  uint32_t _pulses;
};

}  // namespace tth
