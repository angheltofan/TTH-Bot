#include "tth/LogQueue.h"

namespace tth {

namespace {

void copyTruncated(char* dest, const char* source, size_t capacity) {
  if (capacity == 0) return;
  size_t i = 0;
  while (i + 1 < capacity && source[i] != '\0') {
    dest[i] = source[i];
    ++i;
  }
  dest[i] = '\0';
}

}  // namespace

LogQueue::LogQueue() : _head(0), _count(0), _drops(0), _criticalDrops(0) {
  _slots[0][0] = '\0';
}

bool LogQueue::pushInternal(const char* message, bool critical) {
  if (message == nullptr) return false;

  // Routine diagnostics stop short of the reserve, so an end-of-turn report
  // always has room even mid-burst. Critical messages may use the whole queue.
  const size_t limit =
      critical ? kLogQueueSlots : (kLogQueueSlots - kLogReservedCriticalSlots);

  if (_count >= limit) {
    // Drop the NEW message rather than the oldest. During a burst the first
    // lines are the ones that explain what happened; the tail is repetition.
    if (critical) {
      ++_criticalDrops;
    } else {
      ++_drops;
    }
    return false;
  }

  const size_t slot = (_head + _count) % kLogQueueSlots;
  copyTruncated(_slots[slot], message, kLogMessageMax);
  ++_count;
  return true;
}

bool LogQueue::push(const char* message) {
  return pushInternal(message, false);
}

bool LogQueue::pushCritical(const char* message) {
  return pushInternal(message, true);
}

const char* LogQueue::peek() const {
  if (_count == 0) return nullptr;
  return _slots[_head];
}

void LogQueue::discardFront() {
  if (_count == 0) return;
  _head = (_head + 1) % kLogQueueSlots;
  --_count;
}

bool LogQueue::pop(char* out, size_t outCapacity) {
  if (_count == 0 || out == nullptr || outCapacity == 0) return false;
  copyTruncated(out, _slots[_head], outCapacity);
  discardFront();
  return true;
}

}  // namespace tth
