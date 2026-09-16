#include "diag/BlockStats.h"

#include <stdio.h>
#include <string.h>

namespace tth {
namespace diag {

BlockStats::Entry BlockStats::_entries[kMaxBlockLabels];
int BlockStats::_used = 0;
LogQueue* BlockStats::_queue = nullptr;

void BlockStats::setQueue(LogQueue* queue) { _queue = queue; }

void BlockStats::record(const char* label, uint32_t elapsedMicros,
                        uint32_t nowMs) {
  if (label == nullptr) return;

  Entry* entry = nullptr;
  for (int i = 0; i < _used; ++i) {
    // Pointer compare first: every call site passes the same string literal,
    // so this hits almost always and costs nothing.
    if (_entries[i].label == label || strcmp(_entries[i].label, label) == 0) {
      entry = &_entries[i];
      break;
    }
  }

  if (entry == nullptr) {
    if (_used >= kMaxBlockLabels) return;  // table full; silently ignore
    entry = &_entries[_used++];
    entry->label = label;
    entry->maxMicros = 0;
    entry->count = 0;
    entry->overCount = 0;
    entry->lastAlertMs = 0;
  }

  ++entry->count;
  if (elapsedMicros > entry->maxMicros) entry->maxMicros = elapsedMicros;

  if (elapsedMicros <= kAlertMicros) return;

  // Genuinely alarming. Announce it, but at most once per window per label, so
  // a repeating fault cannot flood the port and become the fault.
  ++entry->overCount;
  const bool firstEver = entry->overCount == 1;
  const bool throttleExpired =
      (nowMs - entry->lastAlertMs) >= kAlertThrottleMs;
  if (!firstEver && !throttleExpired) return;

  entry->lastAlertMs = nowMs;
  if (_queue == nullptr) return;

  char line[kLogMessageMax];
  snprintf(line, sizeof(line), "[block] %s took %lu us (over %lu us, %lu times)",
           label, static_cast<unsigned long>(elapsedMicros),
           static_cast<unsigned long>(kAlertMicros),
           static_cast<unsigned long>(entry->overCount));
  _queue->push(line);
}

void BlockStats::reportAndReset(LogQueue& queue) {
  if (_used == 0) return;

  // Slowest first, so the interesting entry is at the front even if the line
  // is later truncated or the queue is saturated.
  for (int i = 0; i < _used; ++i) {
    int slowest = i;
    for (int j = i + 1; j < _used; ++j) {
      if (_entries[j].maxMicros > _entries[slowest].maxMicros) slowest = j;
    }
    if (slowest != i) {
      const Entry temp = _entries[i];
      _entries[i] = _entries[slowest];
      _entries[slowest] = temp;
    }
  }

  char line[kLogMessageMax];
  int offset = snprintf(line, sizeof(line), "[blocks]");
  for (int i = 0; i < _used && offset > 0 &&
                  static_cast<size_t>(offset) < sizeof(line);
       ++i) {
    // Only entries worth looking at; a sub-millisecond operation is noise.
    if (_entries[i].maxMicros < 1000) continue;
    offset += snprintf(line + offset, sizeof(line) - static_cast<size_t>(offset),
                       " %s=%luus", _entries[i].label,
                       static_cast<unsigned long>(_entries[i].maxMicros));
  }
  queue.push(line);

  // Per-interval maxima, so one early spike cannot mask a later regression.
  for (int i = 0; i < _used; ++i) {
    _entries[i].maxMicros = 0;
    _entries[i].count = 0;
  }
}

}  // namespace diag
}  // namespace tth
