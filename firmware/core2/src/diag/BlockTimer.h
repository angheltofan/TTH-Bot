#pragma once

#include <Arduino.h>
#include <stdint.h>

#include "diag/BlockStats.h"

namespace tth {
namespace diag {

// Times the enclosing scope and ACCUMULATES the result.
//
// It no longer prints. Printing per occurrence was what turned the diagnostic
// into the fault: the face render legitimately takes ~12.8 ms on a full
// transition, and a serial line on every such frame cost more than the thing
// being measured. See BlockStats for what happens to the numbers instead.
//
// Cost when nothing is wrong: two micros() calls, a short table lookup and a
// compare. No allocation, no formatting, no I/O.
class BlockTimer {
 public:
  explicit BlockTimer(const char* label)
      : _label(label), _startMicros(micros()) {}

  ~BlockTimer() {
    BlockStats::record(_label, micros() - _startMicros, millis());
  }

  BlockTimer(const BlockTimer&) = delete;
  BlockTimer& operator=(const BlockTimer&) = delete;

 private:
  const char* _label;
  const uint32_t _startMicros;
};

}  // namespace diag
}  // namespace tth

#define TTH_CONCAT_INNER(a, b) a##b
#define TTH_CONCAT(a, b) TTH_CONCAT_INNER(a, b)

// Times the enclosing scope. Results appear in the heartbeat's [blocks] line.
#define TTH_TIME_BLOCK(label) \
  ::tth::diag::BlockTimer TTH_CONCAT(_tthBlockTimer, __LINE__)(label)
