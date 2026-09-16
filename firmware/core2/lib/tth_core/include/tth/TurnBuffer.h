#pragma once

#include <stdint.h>

#include <atomic>

// The recording buffer for one push-to-talk turn.
//
// PREALLOCATED, NEVER RESIZED
//
// The storage is allocated ONCE at start-up (in PSRAM, by the device layer)
// and handed here via attach(). Nothing in this class allocates, frees or
// resizes -- allocating while a turn is in progress would be a latency spike
// in the middle of the one operation that must not stutter, and a failure
// there would lose audio the child has already spoken.
//
// The buffer is deliberately not circular. Overwriting the start of a turn
// would silently corrupt it; when the capacity is reached the recording stops
// instead, with an explicit reason, and any samples that did not fit are
// counted rather than quietly dropped.
//
// TWO INVARIANTS FOR READERS (Phase 5)
//
// Since Phase 5 the buffer has readers other than the capture that fills it:
// TurnStreamer sends it on while it is still being recorded, and the loopback
// mock plays it back afterwards. Both rules below are enforced here, not left
// to callers' good behaviour.
//
//  1. COMMITTED-LENGTH BOUNDARY. append() copies the samples FIRST and only
//     then publishes the new length with a release store. Readers see only
//     committedSamples(), read with an acquire load. So a reader can never
//     observe a length that covers samples which are not fully written.
//
//  2. LIFETIME. A reader takes a lease with retain() and gives it back with
//     release(). While any lease is held, reset() REFUSES to clear the buffer
//     and returns false. A new recording therefore cannot start over audio
//     that is still being streamed or played -- it must wait until every
//     reader has consumed what it needs, or explicitly abandoned the turn.
//
// Portable: the device passes in a pointer, so this whole class is testable on
// the host against ordinary memory.

namespace tth {

class TurnBuffer {
 public:
  TurnBuffer();

  TurnBuffer(const TurnBuffer&) = delete;
  TurnBuffer& operator=(const TurnBuffer&) = delete;

  // Hands over the preallocated storage. Called once, from start-up.
  void attach(int16_t* storage, uint32_t capacitySamples);

  // Clears the contents for a new turn, INCLUDING the per-turn high-water
  // mark. Every figure a turn summary reports must describe that turn alone;
  // a stale high-water value from a previous, longer turn is worse than no
  // value at all. The session high-water mark survives, for judging whether
  // the capacity is right.
  //
  // Returns false, and changes NOTHING, while a reader holds a lease.
  bool reset();

  // Copies up to `count` samples, stopping at capacity, then commits them.
  // Returns how many were actually stored; the shortfall is the caller's to
  // report, never silently discarded.
  uint32_t append(const int16_t* samples, uint32_t count);

  // --- readers ---------------------------------------------------------------

  // Samples that are fully written and safe to read: [0, committedSamples()).
  // This is the ONLY length a reader may use.
  uint32_t committedSamples() const {
    return _committed.load(std::memory_order_acquire);
  }

  // Takes a lease that blocks reset(). Returns false if nothing is attached.
  bool retain();
  // Gives a lease back. Releasing more than was retained is ignored and
  // counted, never allowed to wrap the count.
  void release();
  bool isRetained() const { return _readers > 0; }
  uint32_t readers() const { return _readers; }
  uint32_t unbalancedReleases() const { return _unbalancedReleases; }

  // --- writer-side figures -----------------------------------------------------

  bool isAttached() const { return _storage != nullptr; }
  bool isFull() const { return _size >= _capacity; }
  uint32_t size() const { return _size; }
  uint32_t capacity() const { return _capacity; }
  uint32_t remaining() const { return _capacity - _size; }
  uint32_t byteSize() const { return _size * 2u; }
  uint32_t byteCapacity() const { return _capacity * 2u; }

  // Largest `size` reached during the CURRENT turn. Reset by reset().
  uint32_t highWaterSamples() const { return _highWater; }

  // Largest `size` reached since attach(), across every turn. Never reset:
  // this is what says whether the capacity is right for real use.
  uint32_t sessionHighWaterSamples() const { return _sessionHighWater; }

  // The captured PCM. Readers must stay below committedSamples().
  const int16_t* data() const { return _storage; }

 private:
  int16_t* _storage;
  uint32_t _capacity;
  // Writer's own count. Only append()/reset() touch it.
  uint32_t _size;
  // The published length readers see. Always <= _size, and only ever raised
  // AFTER the samples it covers have been written.
  std::atomic<uint32_t> _committed;
  uint32_t _highWater;
  uint32_t _sessionHighWater;
  uint32_t _readers;
  uint32_t _unbalancedReleases;
};

}  // namespace tth
