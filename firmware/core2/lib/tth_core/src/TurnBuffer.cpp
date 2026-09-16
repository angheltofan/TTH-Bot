#include "tth/TurnBuffer.h"

namespace tth {

TurnBuffer::TurnBuffer()
    : _storage(nullptr),
      _capacity(0),
      _size(0),
      _committed(0),
      _highWater(0),
      _sessionHighWater(0),
      _readers(0),
      _unbalancedReleases(0) {}

void TurnBuffer::attach(int16_t* storage, uint32_t capacitySamples) {
  _storage = storage;
  _capacity = (storage == nullptr) ? 0 : capacitySamples;
  _size = 0;
  _committed.store(0, std::memory_order_release);
  _highWater = 0;
  _sessionHighWater = 0;
  _readers = 0;
}

bool TurnBuffer::reset() {
  // A reader is still working through this audio. Clearing it now would hand
  // that reader the start of the NEXT turn in place of the rest of this one.
  if (_readers > 0) return false;

  _size = 0;
  _committed.store(0, std::memory_order_release);
  // Per-turn, so it goes with the contents. The session figure does not.
  _highWater = 0;
  return true;
}

uint32_t TurnBuffer::append(const int16_t* samples, uint32_t count) {
  if (_storage == nullptr || samples == nullptr || count == 0) return 0;

  const uint32_t space = remaining();
  const uint32_t toCopy = (count < space) ? count : space;

  // 1. WRITE the samples...
  for (uint32_t i = 0; i < toCopy; ++i) {
    _storage[_size + i] = samples[i];
  }
  _size += toCopy;

  // 2. ...THEN PUBLISH the new length. The release store orders every write
  // above before it, so a reader that loads this length (acquire) is
  // guaranteed to see all of the samples it covers.
  _committed.store(_size, std::memory_order_release);

  if (_size > _highWater) _highWater = _size;
  if (_size > _sessionHighWater) _sessionHighWater = _size;

  return toCopy;
}

bool TurnBuffer::retain() {
  if (_storage == nullptr) return false;
  ++_readers;
  return true;
}

void TurnBuffer::release() {
  if (_readers == 0) {
    ++_unbalancedReleases;
    return;
  }
  --_readers;
}

}  // namespace tth
