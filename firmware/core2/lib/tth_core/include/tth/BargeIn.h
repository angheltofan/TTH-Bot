#pragma once

#include <stdint.h>

#include "tth/StageTimer.h"

// The barge-in sequence: the child presses push-to-talk while the robot is
// speaking.
//
// The ORDER is the whole point, so it lives here, in portable code, where the
// tests can record it -- rather than being implied by the order of lines in
// App:
//
//   1. stop accepting playback chunks
//   2. stop the speaker, and wait (across loop iterations) until it is quiet
//   3. cancel the turn source -- nothing from the old turn is delivered after
//   4. release speaker ownership
//   5. acquire and start the microphone
//   6. LISTENING -- only if step 5 succeeded
//
// Half duplex makes this safe precisely because it is sequential: the
// microphone is never started while the speaker could still be using the
// shared I2S hardware.
//
// If the button is released while the speaker is still draining, step 5 is
// skipped: starting a capture nobody is holding would record until the 45 s
// ceiling.
//
// Each step is a named stage, so the complete speaker-to-microphone transition
// appears in the [blocks] breakdown as a measured figure.

namespace tth {

class IBargeInOps {
 public:
  virtual ~IBargeInOps() {}
  virtual void stopAcceptingPlayback() = 0;  // 1
  virtual void stopSpeaker(uint32_t nowMs) = 0;  // 2 (request)
  virtual bool speakerQuiet() = 0;               // 2 (polled)
  virtual void cancelTurnSource() = 0;           // 3
  virtual void releaseSpeaker() = 0;             // 4
  virtual bool startCapture(uint32_t nowMs) = 0;  // 5
  virtual bool stillHeld() const = 0;            // guard for 5
};

enum class BargeInResult : uint8_t {
  Idle = 0,
  // The speaker is still draining; call poll() again next loop.
  Pending,
  // The microphone is recording. The caller enters LISTENING now.
  Listening,
  // The button was let go before the microphone could start. Nothing is
  // owned; the caller returns to READY.
  ReleasedEarly,
  // The microphone could not start. Nothing is owned; the caller enters ERROR.
  Failed,
};

const char* toString(BargeInResult result);

class BargeIn {
 public:
  explicit BargeIn(IBargeInOps& ops);

  void setStageTimer(IStageTimer* timer) { _timer = timer; }

  // Steps 1 and 2 (request). Returns false if a barge-in is already running.
  bool begin(uint32_t nowMs);

  // Steps 2 (wait) to 5. Non-blocking.
  BargeInResult poll(uint32_t nowMs);

  bool isActive() const { return _active; }

  // Press to microphone started (or abandoned), wall time in microseconds,
  // spread across loop iterations. Zero if no stage timer is attached.
  uint32_t lastTotalMicros() const { return _lastTotalMicros; }
  // How long step 2 waited for the speaker to go quiet.
  uint32_t lastDrainMs() const { return _lastDrainMs; }
  uint32_t count() const { return _count; }

 private:
  void finish(uint32_t nowMs);

  IBargeInOps& _ops;
  IStageTimer* _timer;
  bool _active;
  uint32_t _startedMicros;
  uint32_t _startedMs;
  uint32_t _lastTotalMicros;
  uint32_t _lastDrainMs;
  uint32_t _count;
};

}  // namespace tth
