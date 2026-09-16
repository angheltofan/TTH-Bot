#pragma once

#include <stdint.h>
#include <stddef.h>

// A bounded, non-blocking queue of diagnostic messages.
//
// WHY
//
// Diagnostics were distorting the very timing they existed to measure. At
// 115200 baud a byte takes 87 us, so a 250-character summary is ~22 ms of
// blocking if the UART buffer fills -- and the heartbeat, the summary and a
// per-frame [block] line together pushed loop iterations to 57 ms. The
// instrument was the fault.
//
// So messages are queued here instead of written, and drained a few bytes at a
// time only when the UART actually has room. Nothing waits for the wire.
//
// THE RULE THIS ENCODES
//
// When the queue is full, messages are DROPPED and counted. That is the
// correct trade: a lost diagnostic line is an inconvenience, whereas blocking
// the cooperative loop delays microphone servicing and risks losing audio the
// child has already spoken. Diagnostics are never allowed to cost audio.
//
// Portable and allocation-free: the storage is a member, so this is testable
// on the host and cannot fragment the heap.

namespace tth {

// Sized for the longest line the firmware emits, with real headroom.
//
// The end-of-turn report is the constraint. At absurd worst case (every
// counter at uint32 maximum, the longest stop reason, the longest owner name)
// SUMMARY2 needs ~160 bytes. 192 would fit it but leave only 32 spare, which a
// single added field in Phase 5 could quietly consume -- and a truncated
// lifecycle report is worse than none, because it still looks authoritative.
// 224 keeps at least a quarter of the slot free, and the whole queue costs
// 5.4 KB of static RAM.
const size_t kLogMessageMax = 224;
const size_t kLogQueueSlots = 24;

// Slots routine diagnostics may NOT use, so a two-line lifecycle report always
// has somewhere to go even when ordinary chatter has saturated the queue.
// Diagnostics are droppable; the record of what the hardware actually did is
// not.
const size_t kLogReservedCriticalSlots = 4;

class LogQueue {
 public:
  LogQueue();

  // Routine diagnostics. Refused once the queue reaches
  // capacity - kLogReservedCriticalSlots, so chatter can never crowd out a
  // lifecycle report. Truncates at kLogMessageMax - 1. Never blocks.
  bool push(const char* message);

  // Lifecycle reports. May use the reserved slots, so it fails only when the
  // queue is genuinely full. Counted separately: a lost critical line is a
  // defect, not an inconvenience.
  bool pushCritical(const char* message);

  // Copies the oldest message into `out` (NUL-terminated) and removes it.
  // Returns false when empty. Never blocks.
  bool pop(char* out, size_t outCapacity);

  // The oldest message, without removing it. Returns nullptr when empty. Used
  // by the device layer to check the message fits in the UART's free space
  // before committing to write it.
  const char* peek() const;
  void discardFront();

  bool isEmpty() const { return _count == 0; }
  bool isFull() const { return _count == kLogQueueSlots; }
  size_t size() const { return _count; }
  size_t capacity() const { return kLogQueueSlots; }

  // Messages lost because the queue was full. Reported in the heartbeat so a
  // gap in the log is visible rather than silent.
  uint32_t drops() const { return _drops; }
  uint32_t criticalDrops() const { return _criticalDrops; }
  void clearDrops() { _drops = 0; _criticalDrops = 0; }

 private:
  char _slots[kLogQueueSlots][kLogMessageMax];
  size_t _head;   // next slot to pop
  size_t _count;
  uint32_t _drops;
  uint32_t _criticalDrops;

  bool pushInternal(const char* message, bool critical);
};

}  // namespace tth
