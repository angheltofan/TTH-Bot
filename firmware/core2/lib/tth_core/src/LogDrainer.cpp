#include "tth/LogDrainer.h"

#include <string.h>

namespace tth {

namespace {

// Appended to every message. Counted as part of the message's byte stream, so
// a line is never considered sent until its newline has gone out too.
const char kLineEnding[2] = {'\r', '\n'};
const size_t kLineEndingLength = 2;

size_t smallest(size_t a, size_t b) { return a < b ? a : b; }

}  // namespace

LogDrainer::LogDrainer(LogQueue& queue, size_t perCallBudget)
    : _queue(queue),
      _perCallBudget(perCallBudget == 0 ? 1 : perCallBudget),
      _headOffset(0) {}

size_t LogDrainer::service(ILogSink& sink) {
  size_t budget = _perCallBudget;
  size_t written = 0;

  while (budget > 0) {
    const char* head = _queue.peek();
    if (head == nullptr) break;

    const size_t bodyLength = strlen(head);
    const size_t totalLength = bodyLength + kLineEndingLength;

    // Defensive: an offset past the end would mean the message was already
    // complete, so retire it rather than looping.
    if (_headOffset >= totalLength) {
      _queue.discardFront();
      _headOffset = 0;
      continue;
    }

    const size_t available = sink.availableForWrite();
    if (available == 0) break;  // nothing to be done this call

    const size_t remaining = totalLength - _headOffset;
    const size_t toWrite = smallest(smallest(available, remaining), budget);
    if (toWrite == 0) break;

    // Only ever the head message, and only ever forwards from its offset, so
    // ordering holds and bytes from two messages can never interleave.
    size_t accepted = 0;
    if (_headOffset < bodyLength) {
      const size_t chunk = smallest(toWrite, bodyLength - _headOffset);
      accepted = sink.write(
          reinterpret_cast<const uint8_t*>(head + _headOffset), chunk);
    } else {
      // Into the trailing CRLF.
      const size_t endingOffset = _headOffset - bodyLength;
      const size_t chunk = smallest(toWrite, kLineEndingLength - endingOffset);
      accepted = sink.write(
          reinterpret_cast<const uint8_t*>(kLineEnding + endingOffset), chunk);
    }

    // A sink that accepts nothing despite reporting room: stop rather than
    // spin. The offset is untouched, so the message resumes next call.
    if (accepted == 0) break;

    _headOffset += accepted;
    written += accepted;
    budget -= accepted;

    // Retire the message ONLY once every byte, newline included, is out.
    if (_headOffset >= totalLength) {
      _queue.discardFront();
      _headOffset = 0;
    }
  }

  return written;
}

}  // namespace tth
