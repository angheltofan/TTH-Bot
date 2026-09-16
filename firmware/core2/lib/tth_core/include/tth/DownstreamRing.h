#pragma once

#include <stdint.h>

#include <atomic>

// The downstream (gateway -> device) audio ring (PHASE6_PLAN §3.3, Step 6.3).
//
// OWNERSHIP
//
//   producer  the network task ONLY: stage() / commit() / write(),
//             freeBytes(), acceptedConnection()
//   consumer  the loop ONLY: acceptConnection(), front(), copyPcm(),
//             popFront()
//
// It is a single-producer / single-consumer ring over caller-supplied storage
// (PSRAM on the device). No locks: each side owns one position and publishes
// it with a release store; the other side reads it with an acquire load.
//
// PUBLICATION
//
// A record is [connection u32][turn u32][pcm bytes u16][magic u16] followed by
// the PCM. stage() copies the WHOLE record past the published end; only
// commit() moves the published end over it. The consumer reads only up to the
// published end, so it can never observe a partially written frame.
//
// SPACE
//
// Space only ever grows from the producer's point of view (only the consumer
// frees it), so "check free, then stage" cannot be invalidated by the other
// side. A record that does not fit is refused whole (NoSpace) -- nothing
// previously written is touched.
//
// CONNECTIONS AND TURNS
//
// Every record carries the connection generation it arrived on and its turn
// id. The consumer publishes the connection it accepts; the producer drops
// frames of any other connection before they reach the ring, and the consumer
// skips any that were written just before a switch. That is what keeps credit
// from one connection from being returned on the next.

namespace tth {

struct RingFrame {
  uint32_t connection;
  uint32_t turn;
  uint32_t pcmBytes;
};

enum class RingWrite : uint8_t { Written = 0, NoSpace, Invalid };

const char* toString(RingWrite result);

class DownstreamRing {
 public:
  static const uint32_t kHeaderBytes = 12;
  static const uint16_t kMagic = 0xA5D1;
  static const uint32_t kMaxFrameBytes = 0xFFFF;

  DownstreamRing();

  // Once, before either side runs. `storageBytes` covers PCM and headers.
  bool attach(uint8_t* storage, uint32_t storageBytes);
  bool isAttached() const { return _storage != nullptr; }
  uint32_t capacity() const { return _capacity; }

  // --- consumer (loop) ---------------------------------------------------------
  void acceptConnection(uint32_t connection);
  // The oldest published record, not consumed. False if there is none.
  bool front(RingFrame& out);
  // Copies PCM of the front record starting at `offset`. Returns bytes copied.
  uint32_t copyPcm(uint32_t offset, uint8_t* dst, uint32_t maxBytes);
  // Removes the front record.
  void popFront();
  // Records whose magic did not match (a defect: everything is then dropped).
  uint32_t corrupt() const { return _corrupt; }

  // --- producer (network task) ---------------------------------------------------
  uint32_t acceptedConnection() const;
  uint32_t freeBytes() const;
  RingWrite stage(uint32_t connection, uint32_t turn, const uint8_t* pcm,
                  uint32_t pcmBytes);
  void commit();
  RingWrite write(uint32_t connection, uint32_t turn, const uint8_t* pcm,
                  uint32_t pcmBytes);

  // --- either side (diagnostics) -------------------------------------------------
  uint32_t usedBytes() const;
  uint32_t highWaterBytes() const;

 private:
  void copyIn(uint32_t position, const uint8_t* src, uint32_t count);
  void copyOut(uint32_t position, uint8_t* dst, uint32_t count) const;

  uint8_t* _storage;
  uint32_t _capacity;
  std::atomic<uint32_t> _writePos;  // published by the producer
  std::atomic<uint32_t> _readPos;   // published by the consumer
  std::atomic<uint32_t> _accepted;
  std::atomic<uint32_t> _highWater;
  // Producer-only.
  uint32_t _stagedEnd;
  bool _staged;
  // Consumer-only.
  uint32_t _corrupt;
};

}  // namespace tth
