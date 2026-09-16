#pragma once

#include <stdint.h>

#include "tth/LogQueue.h"

namespace tth {
namespace diag {

// Aggregate timing for operations that can block the cooperative loop.
//
// THE PROBLEM WITH THE FIRST VERSION
//
// TTH_TIME_BLOCK printed a line every time an operation exceeded 5 ms. The
// face render legitimately takes ~12.8 ms on a full transition, so it printed
// on every such frame -- and those lines, at 115200 baud, became the blocking
// the instrument was supposed to be measuring. A diagnostic that changes the
// measurement is worse than none.
//
// So timings are now ACCUMULATED, not printed. Each label keeps its maximum
// and a count; the heartbeat prints the table. An immediate line is emitted
// only for something genuinely alarming (over kAlertMicros) and only once per
// label per throttle window, so a repeating fault still announces itself
// without flooding.
const uint32_t kAlertMicros = 20000;
const uint32_t kAlertThrottleMs = 5000;
const int kMaxBlockLabels = 16;

class BlockStats {
 public:
  // Records one measurement. Cheap: a short linear search plus a compare.
  static void record(const char* label, uint32_t elapsedMicros,
                     uint32_t nowMs);

  // Queues the accumulated table, slowest first, then clears the maxima.
  // Called from the heartbeat, never from the timed paths themselves.
  static void reportAndReset(LogQueue& queue);

  // Where alert lines go. Set once at start-up.
  static void setQueue(LogQueue* queue);

 private:
  struct Entry {
    const char* label;
    uint32_t maxMicros;
    uint32_t count;
    uint32_t overCount;
    uint32_t lastAlertMs;
  };

  static Entry _entries[kMaxBlockLabels];
  static int _used;
  static LogQueue* _queue;
};

}  // namespace diag
}  // namespace tth
