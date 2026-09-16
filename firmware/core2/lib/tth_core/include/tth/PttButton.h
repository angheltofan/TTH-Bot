#pragma once

#include <stdint.h>

// Portable push-to-talk button logic: debouncing, one-shot edges and the
// maximum-hold safety timeout.
//
// This class holds ALL of the timing behaviour, and both IPushToTalkInput
// implementations delegate to it. That is deliberate: the capacitive touch
// button and the SW201 differ only in how a raw "is it down right now?"
// boolean is obtained, and keeping everything else here means the two inputs
// cannot behave differently by accident -- which is exactly what the
// "swap by configuration alone" requirement demands.
//
// It takes time as a parameter rather than reading a clock, so every timing
// rule below is directly unit-testable on the host with no hardware and no
// waiting.

namespace tth {

class PttButton {
 public:
  // debounceMs: how long the raw level must be stable before it is believed.
  //             Pass 0 for an input that is already debounced upstream (the
  //             capacitive touch button is debounced inside M5Unified).
  // maxHoldMs:  safety ceiling on a single hold. A stuck button, or a child
  //             leaning on the device, must not be able to pin the microphone
  //             open indefinitely.
  PttButton(uint32_t debounceMs, uint32_t maxHoldMs);

  // Feed the raw, un-debounced level once per loop.
  void update(bool rawDown, uint32_t nowMs);

  bool isHeld() const { return _stable; }

  // One-shot edges: each returns true at most once per event.
  bool consumePress();
  bool consumeRelease();

  bool lastReleaseWasForced() const { return _lastReleaseForced; }

  // Duration of the most recently COMPLETED hold, in ms. Captured at the
  // moment the release edge is generated, so it stays correct however long
  // the caller takes to consume that edge.
  uint32_t lastHoldMs() const { return _lastHoldMs; }

  // Duration of the current hold, or 0 when not held.
  uint32_t heldForMs(uint32_t nowMs) const;

  // Drops any pending edges and returns to the idle state without emitting
  // events. Intended for teardown, not for normal operation.
  void reset();

 private:
  const uint32_t _debounceMs;
  const uint32_t _maxHoldMs;

  bool _rawLast;
  bool _stable;
  uint32_t _lastRawChangeMs;
  uint32_t _pressStartedMs;

  bool _pressEdge;
  bool _releaseEdge;
  bool _lastReleaseForced;
  uint32_t _lastHoldMs;

  // Set when the maximum-hold timeout fires. While set, the input is treated
  // as released no matter what the hardware says, until the button is
  // genuinely let go. Without this a held-down or failed-short button would
  // re-trigger a fresh press every maxHoldMs forever.
  bool _lockedOut;
};

}  // namespace tth
