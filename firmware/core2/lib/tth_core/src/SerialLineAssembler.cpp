#include "tth/SerialLineAssembler.h"

#include <string.h>

namespace tth {

namespace {

SerialEvent none() {
  SerialEvent event;
  event.type = SerialEventType::None;
  event.key = 0;
  event.line = nullptr;
  event.length = 0;
  return event;
}

}  // namespace

SerialLineAssembler::SerialLineAssembler()
    : _length(0), _mode(Mode::Keys), _lastByteMs(0) {
  wipe();
}

void SerialLineAssembler::wipe() {
  memset(_buffer, 0, sizeof(_buffer));
  _length = 0;
}

bool SerialLineAssembler::isWiped() const {
  for (size_t i = 0; i < sizeof(_buffer); ++i) {
    if (_buffer[i] != 0) return false;
  }
  return true;
}

void SerialLineAssembler::poll(uint32_t nowMs) {
  if (_mode == Mode::Keys) return;
  if (nowMs - _lastByteMs >= kIdleTimeoutMs) {
    wipe();
    _mode = Mode::Keys;
  }
}

SerialEvent SerialLineAssembler::feed(uint8_t byte, uint32_t nowMs) {
  SerialEvent event = none();
  const bool newline = (byte == '\r' || byte == '\n');

  switch (_mode) {
    case Mode::Keys:
      if (byte == static_cast<uint8_t>(kLinePrefix)) {
        wipe();
        _mode = Mode::Line;
        _lastByteMs = nowMs;
      } else if (!newline) {
        event.type = SerialEventType::Key;
        event.key = static_cast<char>(byte);
      }
      return event;

    case Mode::Line:
      _lastByteMs = nowMs;
      if (newline) {
        _buffer[_length] = '\0';
        _mode = Mode::Keys;
        event.type = SerialEventType::Line;
        event.line = _buffer;
        event.length = _length;
        return event;
      }
      if (_length >= kMaxLine) {
        wipe();
        _mode = Mode::Discard;
        event.type = SerialEventType::Overflow;
        return event;
      }
      _buffer[_length++] = static_cast<char>(byte);
      return event;

    case Mode::Discard:
      _lastByteMs = nowMs;
      if (newline) _mode = Mode::Keys;
      return event;
  }
  return event;
}

}  // namespace tth
