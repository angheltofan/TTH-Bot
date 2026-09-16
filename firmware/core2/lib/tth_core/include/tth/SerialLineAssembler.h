#pragma once

#include <stddef.h>
#include <stdint.h>

// Splits serial input into single diagnostic keys and '!'-prefixed command
// lines (Step 6.1).
//
// The existing diagnostics act on single keys the moment they arrive (r, l,
// w, s, e, o, a, b, m, p, x, g, ?). Provisioning needs whole lines — and
// typing "prov" must not trigger the 'p' key. So a '!' switches to line mode:
// bytes are buffered, not acted on, until a newline. Everything else behaves
// exactly as before.
//
// Bounded: a line longer than kMaxLine is discarded up to its newline and
// reported once. An abandoned line times out back to key mode. The owner calls
// wipe() after handling a line, since it may carry a credential.
//
// Portable and clock-injected.

namespace tth {

enum class SerialEventType : uint8_t { None = 0, Key, Line, Overflow };

struct SerialEvent {
  SerialEventType type;
  char key;          // Key
  const char* line;  // Line: NUL-terminated, valid until the next feed/wipe
  size_t length;     // Line
};

class SerialLineAssembler {
 public:
  static const size_t kMaxLine = 480;
  static const char kLinePrefix = '!';
  static const uint32_t kIdleTimeoutMs = 30000;

  SerialLineAssembler();

  SerialEvent feed(uint8_t byte, uint32_t nowMs);

  // Abandons a partial line after kIdleTimeoutMs without input.
  void poll(uint32_t nowMs);

  // Zeroes the line buffer.
  void wipe();

  bool inLine() const { return _mode != Mode::Keys; }
  bool isWiped() const;

 private:
  enum class Mode : uint8_t { Keys, Line, Discard };

  char _buffer[kMaxLine + 1];
  size_t _length;
  Mode _mode;
  uint32_t _lastByteMs;
};

}  // namespace tth
