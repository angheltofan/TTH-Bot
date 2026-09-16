#include "tth/PttButton.h"

namespace tth {

PttButton::PttButton(uint32_t debounceMs, uint32_t maxHoldMs)
    : _debounceMs(debounceMs),
      _maxHoldMs(maxHoldMs),
      _rawLast(false),
      _stable(false),
      _lastRawChangeMs(0),
      _pressStartedMs(0),
      _pressEdge(false),
      _releaseEdge(false),
      _lastReleaseForced(false),
      _lastHoldMs(0),
      _lockedOut(false) {}

void PttButton::update(bool rawDown, uint32_t nowMs) {
  // Lock-out is cleared by a GENUINE release, so this is checked against the
  // real hardware level before any masking below.
  if (_lockedOut) {
    if (!rawDown) _lockedOut = false;
    // Until then the button is treated as up regardless of what it reports.
    rawDown = false;
  }

  if (rawDown != _rawLast) {
    _rawLast = rawDown;
    _lastRawChangeMs = nowMs;
  }

  // Believe the raw level only once it has been stable long enough. With
  // _debounceMs == 0 this accepts the change on the same iteration.
  if (_stable != _rawLast && (nowMs - _lastRawChangeMs) >= _debounceMs) {
    _stable = _rawLast;
    if (_stable) {
      _pressStartedMs = nowMs;
      _pressEdge = true;
      _lastReleaseForced = false;
    } else {
      _releaseEdge = true;
      _lastReleaseForced = false;
      _lastHoldMs = nowMs - _pressStartedMs;
    }
  }

  // Safety ceiling. Emitted immediately rather than debounced: it is generated
  // here, not observed on a pin, so there is nothing to settle.
  if (_stable && (nowMs - _pressStartedMs) >= _maxHoldMs) {
    _stable = false;
    _releaseEdge = true;
    _lastReleaseForced = true;
    _lastHoldMs = nowMs - _pressStartedMs;
    _lockedOut = true;
    // Keep _rawLast in step so that letting go after a forced release does not
    // register as a second release edge.
    _rawLast = false;
    _lastRawChangeMs = nowMs;
  }
}

bool PttButton::consumePress() {
  const bool edge = _pressEdge;
  _pressEdge = false;
  return edge;
}

bool PttButton::consumeRelease() {
  const bool edge = _releaseEdge;
  _releaseEdge = false;
  return edge;
}

uint32_t PttButton::heldForMs(uint32_t nowMs) const {
  if (!_stable) return 0;
  return nowMs - _pressStartedMs;
}

void PttButton::reset() {
  _rawLast = false;
  _stable = false;
  _lastRawChangeMs = 0;
  _pressStartedMs = 0;
  _pressEdge = false;
  _releaseEdge = false;
  _lastReleaseForced = false;
  _lastHoldMs = 0;
  _lockedOut = false;
}

}  // namespace tth
