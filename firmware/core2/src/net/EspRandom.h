#pragma once

#include <esp_system.h>

#include "tth/RandomSource.h"

namespace tth {

// The ESP32 hardware random number generator, for the reset confirmation
// code and the Wi-Fi backoff jitter.
class EspRandom : public IRandomSource {
 public:
  uint32_t next() override { return esp_random(); }
};

}  // namespace tth
