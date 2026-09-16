#pragma once

#include <stdint.h>

#include "tth/GatewayProtocol.h"

// Every frame the device sends to the gateway, in ONE queue with a structural
// send fence (docs/PHASE6_PLAN.md §3.1-3.2).
//
// WHY ONE QUEUE
//
// Separate audio and control queues would let `turn_end` overtake audio that
// was still waiting to be sent. Here a turn's frames share one FIFO lane:
//
//   turn_start(N) · audio(N)... · turn_end(N)
//
// and an item leaves the queue only when completeSend() confirms that the
// WHOLE frame went out. With exactly one sender, `turn_end(N)` is therefore
// transmitted only after every audio frame before it was sent successfully --
// not merely copied into the queue.
//
// Priority lane: `credit`, `ping` and `cancel`. They go out before the next
// ordered item, but only at a frame boundary -- never inside a frame.
//
// CANCEL (§3.2) is applied by the sender at the next frame boundary:
//   1. every UNSENT ordered item of turn N is purged;
//   2. if turn_start(N) never reached the wire, the gateway never heard of N
//      and the cancel is dropped;
//   3. otherwise cancel(N) goes out on the priority lane -- after whatever of
//      N was already sent, and before any item of a later turn.
//
// THREADING: this class is not itself thread-safe. On the device the loop
// (producer) and the NetSender task (consumer) share it under one mutex; the
// host tests drive it directly.
//
// Portable; fixed storage, no heap.

namespace tth {

enum class OutKind : uint8_t { TurnStart, Audio, TurnEnd, Cancel, Credit, Ping };
enum class OutLane : uint8_t { Ordered, Priority };
enum class OutPush : uint8_t { Accepted = 0, Busy, Invalid };

const char* toString(OutKind kind);

// Large enough for the biggest frame: 8-byte header + 640 B of PCM.
const uint32_t kOutboundSlotBytes =
    static_cast<uint32_t>(wire::kAudioHeaderBytes) + wire::kMaxUpPcmBytes;

struct OutboundItem {
  OutKind kind;
  OutLane lane;
  bool binary;
  uint32_t turn;
  uint32_t value;  // the byte count of a credit message (for coalescing)
  uint32_t length;
  uint8_t bytes[kOutboundSlotBytes];
};

struct OutboundCounters {
  uint32_t sent;
  uint32_t busy;
  uint32_t purged;
  uint32_t cancelsSent;
  uint32_t cancelsDropped;
  uint32_t failedSends;
};

class OutboundQueue {
 public:
  static const uint32_t kSlots = 32;
  // Audio may not use the last kReservedSlots, so turn_start, turn_end,
  // cancel and credit can never be starved by a burst of audio.
  static const uint32_t kReservedSlots = 4;
  static const uint32_t kMaxPendingCancels = 4;
  static const uint32_t kWireMemory = 16;

  OutboundQueue();

  // --- producer (loop) ---------------------------------------------------------

  // Invalid if a turn is already open, or turn 0.
  OutPush pushTurnStart(uint32_t turn);
  // Invalid unless `turn` is the open turn, or 0 / >320 samples. Busy when
  // only reserved slots remain -- the caller retries (the TurnBuffer holds
  // the audio, so nothing is lost).
  OutPush pushAudio(uint32_t turn, const int16_t* pcm, uint32_t samples);
  // Closes the open turn. `frames`/`bytes` are the turn's totals, which the
  // gateway checks against what it received.
  OutPush pushTurnEnd(uint32_t turn, uint32_t frames, uint32_t bytes);
  // Applied at the next frame boundary (see above). Closes the turn if open.
  void requestCancel(uint32_t turn);
  // Adds to an unsent credit message if there is one.
  OutPush pushCredit(uint32_t bytes);
  OutPush pushPing(uint32_t ts);

  // --- consumer (NetSender) ------------------------------------------------------

  // The next item to send, at a frame boundary, or nullptr. Pending cancels
  // are applied first. The item stays in the queue, marked in flight.
  const OutboundItem* beginSend();
  // The in-flight item was sent completely: remove it.
  void completeSend();
  // The send failed or timed out: the link is failing. Everything is
  // dropped and the turn state is cleared.
  void failSend();

  // A new connection (Step 6.3): everything dropped, turn state cleared, and
  // no failed send counted. Called by the sender, at a frame boundary.
  void reset();

  // Most items ever queued at once (diagnostics).
  uint32_t highWater() const { return _highWater; }

  uint32_t size() const { return _orderedCount + _priorityCount; }
  uint32_t freeSlots() const { return _freeCount; }
  uint32_t openTurn() const { return _openTurn; }
  bool inFlight() const { return _inFlight != kNone; }
  const OutboundCounters& counters() const { return _counters; }

 private:
  static const uint32_t kNone = 0xFFFFFFFFu;

  uint32_t takeSlot(bool mayUseReserve);
  void releaseSlot(uint32_t slot);
  void appendOrdered(uint32_t slot);
  void appendPriority(uint32_t slot);
  void applyPendingCancels();
  bool reachedWire(uint32_t turn) const;
  void rememberWire(uint32_t turn);

  OutboundItem _items[kSlots];
  uint32_t _free[kSlots];
  uint32_t _freeCount;

  uint32_t _ordered[kSlots];
  uint32_t _orderedHead;
  uint32_t _orderedCount;
  uint32_t _priority[kSlots];
  uint32_t _priorityHead;
  uint32_t _priorityCount;

  uint32_t _inFlight;  // slot index, or kNone
  bool _inFlightPriority;

  uint32_t _openTurn;
  uint32_t _pendingCancels[kMaxPendingCancels];
  uint32_t _pendingCancelCount;
  uint32_t _wire[kWireMemory];  // turns whose turn_start reached the wire
  uint32_t _wireCount;
  uint32_t _wireHead;

  OutboundCounters _counters;
  uint32_t _highWater;
};

}  // namespace tth
