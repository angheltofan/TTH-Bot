#pragma once

#include <stdint.h>

#include "tth/LogDrainer.h"
#include "tth/LogQueue.h"

namespace tth {
namespace diag {

// Queued, non-blocking serial diagnostics.
//
// Every diagnostic line in the running firmware goes through here instead of
// Serial.printf, so a write can never stall waiting for the wire.
//
// The draining itself lives in the portable LogDrainer, which writes messages
// in PIECES. That matters: the ESP32 UART TX FIFO is 128 bytes, so
// availableForWrite() never reports more than that, and the earlier
// "only write a line if the whole thing fits" rule meant the 130- and 162-byte
// summary lines could never be written at all -- they blocked the head of the
// queue permanently and starved everything behind them.
//
// Baud stays at 115200. The fix is partial writing, not a bigger buffer: this
// works whatever the UART buffer capacity happens to be.
class SerialLog {
 public:
  SerialLog();

  // printf-style. Formats into the bounded queue. Never blocks; on a full
  // queue the message is dropped and counted.
  void printf(const char* format, ...) __attribute__((format(printf, 2, 3)));

  // Writes whatever fits right now, up to the per-call byte budget. Call once
  // per loop iteration. Returns the microseconds spent, so the caller reports
  // it separately from real application blocking.
  //
  // This measures ONE partial write, not the time to transmit a whole line.
  uint32_t service();

  uint32_t drops() const { return _queue.drops(); }
  uint32_t maxWriteMicros() const { return _maxWriteMicros; }
  void resetStats() { _maxWriteMicros = 0; }
  size_t pending() const { return _queue.size(); }
  bool hasPartialMessage() const { return _drainer.hasPartialMessage(); }

  LogQueue& queue() { return _queue; }

 private:
  LogQueue _queue;
  LogDrainer _drainer;
  uint32_t _maxWriteMicros;
};

// THE process-wide log.
//
// Every producer -- App, the microphone adapter, the audio device, the block
// timers -- must write through this one instance. Mixing queued and direct
// Serial writes is what put the log out of order: direct writes appeared
// immediately while queued ones waited, so "[mic] recording" and
// "[app] state ..." overtook the "[ptt] PRESS" that caused them. One queue
// means one FIFO, and therefore true event order without needing sequence
// numbers.
SerialLog& log();

}  // namespace diag
}  // namespace tth
