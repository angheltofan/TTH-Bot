#pragma once

#include <stdint.h>

// A source of random numbers, injected so portable code stays testable.
// The device uses the ESP32 hardware RNG (src/net/EspRandom.h).

namespace tth {

class IRandomSource {
 public:
  virtual ~IRandomSource() {}
  virtual uint32_t next() = 0;
};

}  // namespace tth
