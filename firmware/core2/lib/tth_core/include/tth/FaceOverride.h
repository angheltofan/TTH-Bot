#pragma once

#include <stdint.h>

#include "tth/FaceState.h"

// Serial-driven visual testing of the face, without touching the production
// state machine.
//
// This is a purely VISUAL override: ConversationStateMachine is never written
// to, so nothing here can leave the firmware in a state the real inputs could
// not have produced.
//
// The override is cleared by the next push-to-talk press, so the device always
// returns to real behaviour by itself. It is portable, and therefore tested.

namespace tth {

class FaceOverride {
 public:
  // cycleIntervalMs: how long each face is shown in automatic cycle mode.
  explicit FaceOverride(uint32_t cycleIntervalMs);

  // Handles one diagnostic key. Returns true if the key was recognised.
  //   r/l/w/s/e/o -> hold that face (o = sleeping)
  //   a           -> cycle through every face
  bool handleKey(char key, uint32_t nowMs);

  // Drops any override and returns the face to the real state machine. Called
  // on every push-to-talk press.
  void clear();

  bool isActive() const { return _mode != Mode::None; }
  bool isCycling() const { return _mode == Mode::Cycle; }

  // The face to draw, or `productionFace` when no override is active.
  FaceState faceFor(FaceState productionFace, uint32_t nowMs);

 private:
  enum class Mode : uint8_t { None, Hold, Cycle };

  const uint32_t _cycleIntervalMs;
  Mode _mode;
  FaceState _held;
  uint32_t _cycleStartedMs;
};

// Fixed speech levels the 'b' diagnostic steps through, so the audio bars can
// be checked at known amplitudes rather than only against the moving envelope.
// Index kFixedLevelCount means "use the simulated envelope instead".
const int kFixedLevelCount = 5;
extern const float kFixedSpeechLevels[kFixedLevelCount];

// Advances the step index, wrapping back to the simulated envelope after the
// loudest fixed level.
int nextFixedLevelIndex(int current);

// The level for a step index, or -1.0f for "use the simulated envelope".
float fixedSpeechLevelFor(int index);

// The order automatic cycle mode walks through. Exposed so the test can assert
// it covers every face exactly once.
const int kFaceCycleLength = 6;
extern const FaceState kFaceCycle[kFaceCycleLength];

}  // namespace tth
