#include "haptics/Haptics.h"

#include <M5Unified.h>

#include "diag/BlockTimer.h"
#include "tth/Config.h"

namespace tth {

namespace {
const uint16_t kDeniedPattern[3] = {TTH_DENIED_PULSE_MS, TTH_DENIED_GAP_MS,
                                    TTH_DENIED_PULSE_MS};
}  // namespace

Haptics::Haptics()
    : _pulse(TTH_VIBRATION_MS), _denied(kDeniedPattern, 3) {}

bool Haptics::pulse(uint32_t nowMs) {
  if (_denied.isActive()) return false;
  if (!_pulse.start(nowMs)) return false;
  // An I2C write to the power management chip. Timed because it is on the
  // loop, not because it is expected to be slow.
  TTH_TIME_BLOCK("vibration.on");
  M5.Power.setVibration(TTH_VIBRATION_LEVEL);
  return true;
}

bool Haptics::denied(uint32_t nowMs) {
  if (_pulse.isActive()) return false;
  if (!_denied.start(nowMs)) return false;
  TTH_TIME_BLOCK("vibration.on");
  M5.Power.setVibration(TTH_VIBRATION_LEVEL);
  return true;
}

void Haptics::poll(uint32_t nowMs) {
  if (_pulse.poll(nowMs)) {
    TTH_TIME_BLOCK("vibration.off");
    M5.Power.setVibration(0);
  }
  switch (_denied.poll(nowMs)) {
    case HapticPattern::Motor::On: {
      TTH_TIME_BLOCK("vibration.on");
      M5.Power.setVibration(TTH_VIBRATION_LEVEL);
      break;
    }
    case HapticPattern::Motor::Off: {
      TTH_TIME_BLOCK("vibration.off");
      M5.Power.setVibration(0);
      break;
    }
    case HapticPattern::Motor::NoChange:
      break;
  }
}

}  // namespace tth
