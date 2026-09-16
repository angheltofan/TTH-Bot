#include "tth/DownstreamCredit.h"

namespace tth {

DownstreamCredit::DownstreamCredit(uint32_t capacityBytes,
                                   uint32_t returnBatchBytes)
    : _capacity(capacityBytes),
      _batch(returnBatchBytes == 0 ? 1u : returnBatchBytes),
      _received(0),
      _consumed(0),
      _returned(0),
      _violations(0),
      _overConsumed(0),
      _desynced(false) {}

bool DownstreamCredit::admit(uint32_t pcmBytes) {
  const uint32_t received = _received.load(std::memory_order_relaxed);
  const uint32_t returned = _returned.load(std::memory_order_acquire);
  // Unsigned differences stay correct across 32-bit wrap.
  const uint64_t after = static_cast<uint64_t>(received - returned) + pcmBytes;
  if (after > _capacity) {
    _violations.fetch_add(1, std::memory_order_relaxed);
    _desynced.store(true, std::memory_order_release);
    return false;
  }
  _received.store(received + pcmBytes, std::memory_order_release);
  return true;
}

void DownstreamCredit::consumed(uint32_t pcmBytes) {
  const uint32_t received = _received.load(std::memory_order_acquire);
  const uint32_t consumed = _consumed.load(std::memory_order_relaxed);
  const uint32_t available = received - consumed;
  if (pcmBytes > available) {
    _overConsumed.fetch_add(1, std::memory_order_relaxed);
    pcmBytes = available;
  }
  _consumed.store(consumed + pcmBytes, std::memory_order_release);
}

uint32_t DownstreamCredit::takeReturn(bool force) {
  const uint32_t consumed = _consumed.load(std::memory_order_relaxed);
  const uint32_t returned = _returned.load(std::memory_order_relaxed);
  const uint32_t pending = consumed - returned;
  if (pending == 0) return 0;
  if (!force && pending < _batch) return 0;
  _returned.store(returned + pending, std::memory_order_release);
  return pending;
}

uint32_t DownstreamCredit::pendingReturn(bool force) const {
  const uint32_t consumed = _consumed.load(std::memory_order_relaxed);
  const uint32_t returned = _returned.load(std::memory_order_relaxed);
  const uint32_t pending = consumed - returned;
  if (pending == 0) return 0;
  if (!force && pending < _batch) return 0;
  return pending;
}

void DownstreamCredit::commitReturn(uint32_t bytes) {
  const uint32_t consumed = _consumed.load(std::memory_order_relaxed);
  const uint32_t returned = _returned.load(std::memory_order_relaxed);
  const uint32_t pending = consumed - returned;
  if (bytes > pending) bytes = pending;
  _returned.store(returned + bytes, std::memory_order_release);
}

void DownstreamCredit::beginConsumerEpoch() {
  _consumed.store(0, std::memory_order_release);
  _returned.store(0, std::memory_order_release);
  _desynced.store(false, std::memory_order_release);
}

void DownstreamCredit::beginProducerEpoch() {
  _received.store(0, std::memory_order_release);
}

uint32_t DownstreamCredit::gatewayRemaining() const {
  const uint32_t outstandingBytes = outstanding();
  return outstandingBytes >= _capacity ? 0 : _capacity - outstandingBytes;
}

uint32_t DownstreamCredit::consumedBytes() const {
  return _consumed.load(std::memory_order_acquire);
}

void DownstreamCredit::reset() {
  _received.store(0, std::memory_order_relaxed);
  _consumed.store(0, std::memory_order_relaxed);
  _returned.store(0, std::memory_order_relaxed);
  _desynced.store(false, std::memory_order_release);
}

uint32_t DownstreamCredit::received() const {
  return _received.load(std::memory_order_acquire);
}
uint32_t DownstreamCredit::returned() const {
  return _returned.load(std::memory_order_acquire);
}
uint32_t DownstreamCredit::outstanding() const {
  return received() - returned();
}
uint32_t DownstreamCredit::inRing() const {
  return received() - _consumed.load(std::memory_order_acquire);
}
uint32_t DownstreamCredit::violations() const {
  return _violations.load(std::memory_order_relaxed);
}
uint32_t DownstreamCredit::overConsumed() const {
  return _overConsumed.load(std::memory_order_relaxed);
}
bool DownstreamCredit::desynced() const {
  return _desynced.load(std::memory_order_acquire);
}

}  // namespace tth
