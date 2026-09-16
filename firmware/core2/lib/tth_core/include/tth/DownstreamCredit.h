#pragma once

#include <stdint.h>

#include <atomic>

// Device-side accounting for the downstream (gateway -> device) audio credit
// (docs/PHASE6_PLAN.md §3.3).
//
// The device grants the gateway `capacity` PCM bytes -- the free space of its
// playback ring. The gateway may send model audio only within that credit;
// the device returns credit as it CONSUMES audio (copied into the player, or
// discarded as stale after a cancel -- so cancelling never leaks credit).
//
// NOTHING HERE WAITS. The receive side runs on the WebSocket event task and
// only checks and counts:
//
//   admit(n) is true iff  received - returned + n <= capacity
//
// A frame that fails it is a CREDIT VIOLATION: counted, flagged `desynced`,
// and dropped by the caller -- the turn fails and the connection is rebuilt,
// because the two sides' counts can no longer be trusted.
//
// Threading: admit() on the network task; consumed()/takeReturn()/reset() on
// the loop task (reset only while the network task is stopped). Each counter
// has a single writer, published with release/acquire.
//
// Portable.

namespace tth {

class DownstreamCredit {
 public:
  explicit DownstreamCredit(uint32_t capacityBytes,
                            uint32_t returnBatchBytes = 3840);

  uint32_t capacity() const { return _capacity; }

  // --- network task --------------------------------------------------------
  bool admit(uint32_t pcmBytes);

  // --- loop task -----------------------------------------------------------
  // Audio left the ring (played or discarded). More than was received is a
  // caller bug: clamped and counted.
  void consumed(uint32_t pcmBytes);
  // Bytes to announce in a `credit` message now: the consumed-but-not-yet-
  // returned amount, once it reaches the batch size (or at all when `force`,
  // e.g. at the end of a turn). 0 = nothing to send.
  uint32_t takeReturn(bool force);
  // A new connection: a fresh grant, all counts zero.
  void reset();

  // --- Step 6.3: two-phase return and per-connection epochs ------------------
  //
  // pendingReturn() says how much could be returned now (same batch rule as
  // takeReturn) WITHOUT marking it returned; commitReturn() marks it only
  // after the `credit` message was actually queued. A full outbound queue can
  // therefore never lose credit.
  uint32_t pendingReturn(bool force) const;
  void commitReturn(uint32_t bytes);
  // Epochs for a new connection, one per owning side. The loop calls
  // beginConsumerEpoch() BEFORE it asks the network task to connect; the
  // network task calls beginProducerEpoch() when it starts that connect.
  void beginConsumerEpoch();  // loop: consumed = returned = 0
  void beginProducerEpoch();  // network task: received = 0
  // Credit the gateway may still spend: capacity - (received - returned).
  uint32_t gatewayRemaining() const;
  uint32_t consumedBytes() const;

  // --- diagnostics ---------------------------------------------------------
  uint32_t received() const;
  uint32_t returned() const;
  uint32_t outstanding() const;  // received - returned (the gateway's view)
  uint32_t inRing() const;       // received - consumed
  uint32_t violations() const;
  uint32_t overConsumed() const;
  bool desynced() const;

 private:
  const uint32_t _capacity;
  const uint32_t _batch;
  std::atomic<uint32_t> _received;
  std::atomic<uint32_t> _consumed;
  std::atomic<uint32_t> _returned;
  std::atomic<uint32_t> _violations;
  std::atomic<uint32_t> _overConsumed;
  std::atomic<bool> _desynced;
};

}  // namespace tth
