#pragma once

#include <stdint.h>
#include <stddef.h>

#include "tth/LogQueue.h"

// Drains the log queue a few bytes at a time, resuming where it left off.
//
// THE DEFECT THIS FIXES: head-of-line blocking
//
// The first version refused to write a message unless the whole line fitted in
// the UART's free space. On ESP32 the TX FIFO is 128 bytes, so
// availableForWrite() never reports more than that -- and the two summary
// lines need 130 and 162 bytes including CRLF. Neither could EVER be written.
// They stuck at the head of the queue permanently, and because a queue drains
// in order, every later message starved behind them. That is exactly what was
// observed: short early lines appeared, capture ran correctly, and then the
// summaries and all subsequent [cap]/[mem]/[blocks] lines simply stopped.
//
// So a message is now written in pieces. An offset tracks how much of the head
// message has gone out; each call writes whatever fits right now, up to a
// strict per-call budget, and the message is removed only once every byte --
// including the newline -- has been accepted.
//
// Ordering is preserved and bytes from different messages are never
// interleaved, because only the head is ever written.
//
// The sink is injected so the whole thing is testable on the host against a
// UART that reports one byte free, or zero, or a different number every call.

namespace tth {

// The byte sink. On the device this is Serial; in tests it is a fake with a
// controllable availableForWrite().
class ILogSink {
 public:
  virtual ~ILogSink() {}

  // How many bytes can be accepted right now without blocking.
  virtual size_t availableForWrite() = 0;

  // Writes up to `length` bytes. Returns how many were actually accepted,
  // which may be fewer. Must never block.
  virtual size_t write(const uint8_t* data, size_t length) = 0;
};

class LogDrainer {
 public:
  // perCallBudget bounds one service() call, so draining a backlog can never
  // turn a single loop iteration into a long write.
  LogDrainer(LogQueue& queue, size_t perCallBudget);

  // Writes what it can and returns the number of bytes written. Never blocks
  // and never exceeds the budget.
  size_t service(ILogSink& sink);

  // True when the head message has been partly written. That message must not
  // be dropped or overwritten while this holds.
  bool hasPartialMessage() const { return _headOffset > 0; }
  size_t headOffset() const { return _headOffset; }
  size_t perCallBudget() const { return _perCallBudget; }

 private:
  LogQueue& _queue;
  const size_t _perCallBudget;
  // Bytes of the head message already accepted, counting the trailing CRLF as
  // part of the message.
  size_t _headOffset;
};

}  // namespace tth
