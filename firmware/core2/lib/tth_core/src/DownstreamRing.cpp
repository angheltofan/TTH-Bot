#include "tth/DownstreamRing.h"

#include <string.h>

namespace tth {

namespace {

void putU32(uint8_t* out, uint32_t value) {
  out[0] = static_cast<uint8_t>(value & 0xFFu);
  out[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
  out[2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
  out[3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
}

uint32_t getU32(const uint8_t* in) {
  return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) |
         (static_cast<uint32_t>(in[2]) << 16) | (static_cast<uint32_t>(in[3]) << 24);
}

}  // namespace

const char* toString(RingWrite result) {
  switch (result) {
    case RingWrite::Written:
      return "written";
    case RingWrite::NoSpace:
      return "no space";
    case RingWrite::Invalid:
      return "invalid";
  }
  return "invalid";
}

DownstreamRing::DownstreamRing()
    : _storage(nullptr),
      _capacity(0),
      _writePos(0),
      _readPos(0),
      _accepted(0),
      _highWater(0),
      _stagedEnd(0),
      _staged(false),
      _corrupt(0) {}

bool DownstreamRing::attach(uint8_t* storage, uint32_t storageBytes) {
  if (storage == nullptr || storageBytes < kHeaderBytes + 2) return false;
  _storage = storage;
  _capacity = storageBytes;
  _writePos.store(0, std::memory_order_release);
  _readPos.store(0, std::memory_order_release);
  _highWater.store(0, std::memory_order_release);
  _stagedEnd = 0;
  _staged = false;
  return true;
}

void DownstreamRing::copyIn(uint32_t position, const uint8_t* src, uint32_t count) {
  const uint32_t start = position % _capacity;
  const uint32_t first = (count < _capacity - start) ? count : _capacity - start;
  memcpy(_storage + start, src, first);
  if (count > first) memcpy(_storage, src + first, count - first);
}

void DownstreamRing::copyOut(uint32_t position, uint8_t* dst, uint32_t count) const {
  const uint32_t start = position % _capacity;
  const uint32_t first = (count < _capacity - start) ? count : _capacity - start;
  memcpy(dst, _storage + start, first);
  if (count > first) memcpy(dst + first, _storage, count - first);
}

// --- consumer ------------------------------------------------------------------------

void DownstreamRing::acceptConnection(uint32_t connection) {
  _accepted.store(connection, std::memory_order_release);
}

bool DownstreamRing::front(RingFrame& out) {
  if (_storage == nullptr) return false;
  const uint32_t r = _readPos.load(std::memory_order_relaxed);
  const uint32_t w = _writePos.load(std::memory_order_acquire);
  if (w - r < kHeaderBytes) return false;
  uint8_t header[kHeaderBytes];
  copyOut(r, header, kHeaderBytes);
  const uint16_t magic =
      static_cast<uint16_t>(header[10] | (static_cast<uint16_t>(header[11]) << 8));
  const uint32_t pcmBytes =
      static_cast<uint32_t>(header[8]) | (static_cast<uint32_t>(header[9]) << 8);
  if (magic != kMagic || w - r < kHeaderBytes + pcmBytes) {
    // Cannot happen with a correct producer. Drop everything published rather
    // than interpret garbage as audio.
    ++_corrupt;
    _readPos.store(w, std::memory_order_release);
    return false;
  }
  out.connection = getU32(header);
  out.turn = getU32(header + 4);
  out.pcmBytes = pcmBytes;
  return true;
}

uint32_t DownstreamRing::copyPcm(uint32_t offset, uint8_t* dst, uint32_t maxBytes) {
  RingFrame frame;
  if (dst == nullptr || !front(frame) || offset >= frame.pcmBytes) return 0;
  const uint32_t available = frame.pcmBytes - offset;
  const uint32_t count = (maxBytes < available) ? maxBytes : available;
  copyOut(_readPos.load(std::memory_order_relaxed) + kHeaderBytes + offset, dst, count);
  return count;
}

void DownstreamRing::popFront() {
  RingFrame frame;
  if (!front(frame)) return;
  const uint32_t r = _readPos.load(std::memory_order_relaxed);
  _readPos.store(r + kHeaderBytes + frame.pcmBytes, std::memory_order_release);
}

// --- producer ----------------------------------------------------------------------------

uint32_t DownstreamRing::acceptedConnection() const {
  return _accepted.load(std::memory_order_acquire);
}

uint32_t DownstreamRing::freeBytes() const {
  if (_storage == nullptr) return 0;
  const uint32_t w = _writePos.load(std::memory_order_relaxed);
  const uint32_t r = _readPos.load(std::memory_order_acquire);
  return _capacity - (w - r);
}

RingWrite DownstreamRing::stage(uint32_t connection, uint32_t turn, const uint8_t* pcm,
                                uint32_t pcmBytes) {
  if (_storage == nullptr || _staged || pcm == nullptr || pcmBytes == 0 ||
      pcmBytes > kMaxFrameBytes) {
    return RingWrite::Invalid;
  }
  const uint32_t w = _writePos.load(std::memory_order_relaxed);
  const uint32_t r = _readPos.load(std::memory_order_acquire);
  const uint64_t need = static_cast<uint64_t>(kHeaderBytes) + pcmBytes;
  if (need > static_cast<uint64_t>(_capacity - (w - r))) return RingWrite::NoSpace;

  uint8_t header[kHeaderBytes];
  putU32(header, connection);
  putU32(header + 4, turn);
  header[8] = static_cast<uint8_t>(pcmBytes & 0xFFu);
  header[9] = static_cast<uint8_t>((pcmBytes >> 8) & 0xFFu);
  header[10] = static_cast<uint8_t>(kMagic & 0xFFu);
  header[11] = static_cast<uint8_t>((kMagic >> 8) & 0xFFu);
  copyIn(w, header, kHeaderBytes);
  copyIn(w + kHeaderBytes, pcm, pcmBytes);
  _stagedEnd = w + static_cast<uint32_t>(need);
  _staged = true;
  return RingWrite::Written;
}

void DownstreamRing::commit() {
  if (!_staged) return;
  _writePos.store(_stagedEnd, std::memory_order_release);
  _staged = false;
  const uint32_t used = _stagedEnd - _readPos.load(std::memory_order_acquire);
  if (used > _highWater.load(std::memory_order_relaxed)) {
    _highWater.store(used, std::memory_order_release);
  }
}

RingWrite DownstreamRing::write(uint32_t connection, uint32_t turn, const uint8_t* pcm,
                                uint32_t pcmBytes) {
  const RingWrite result = stage(connection, turn, pcm, pcmBytes);
  if (result == RingWrite::Written) commit();
  return result;
}

uint32_t DownstreamRing::usedBytes() const {
  return _writePos.load(std::memory_order_acquire) -
         _readPos.load(std::memory_order_acquire);
}

uint32_t DownstreamRing::highWaterBytes() const {
  return _highWater.load(std::memory_order_acquire);
}

}  // namespace tth
