#pragma once

#include <stdint.h>

#include "tth/FaceFrame.h"
#include "tth/FaceState.h"

// Everything the face DOES, as opposed to where its parts are drawn.
//
// Portable and clock-injected: FaceAnimator never reads a timer, it is handed
// the current millisecond. That makes blink intervals, blink duration, pupil
// drift and the speech envelope all directly testable on the host, with no
// display and no waiting.
//
// The timings are ported from the Flutter face (robot_face.dart and
// robot_eyes.dart) so the two embodiments feel like the same character.

namespace tth {
namespace face {

// --- blink (RobotFace._scheduleNextBlink / _blink) --------------------------

// Randomised, not a fixed period, so idle blinking reads as alive rather than
// mechanical. Flutter uses 2500 + rand(3500).
const uint32_t kBlinkMinIntervalMs = 2500;
const uint32_t kBlinkMaxIntervalMs = 6000;

const uint32_t kBlinkCloseMs = 150;
const uint32_t kBlinkOpenMs = 150;

// Occasional natural double-blink, the way people sometimes actually blink.
const uint32_t kDoubleBlinkGapMs = 160;
const int kDoubleBlinkPercent = 22;

// --- idle pupil drift (RobotEyes._scheduleNextIdleMove) ---------------------
//
// "Occasional very small eye movements", not eyes that constantly dart about.
const uint32_t kDriftMinIntervalMs = 4000;
const uint32_t kDriftMaxIntervalMs = 8000;
const uint32_t kDriftDurationMs = 1800;
const float kDriftMaxX = 0.08f;
const float kDriftMaxY = 0.05f;

// --- waiting "thinking" sweep ----------------------------------------------
const uint32_t kThinkingSweepPeriodMs = 2600;
const float kThinkingSweepX = 0.18f;
const float kThinkingLookUpY = -0.10f;

// --- pupil ------------------------------------------------------------------
const float kPupilTravel = 0.30f;        // of the eye radius
const float kPupilRadiusRatio = 0.42f;   // of the eye radius
const float kPupilVisibleOpenness = 0.35f;

// --- the fixed smile --------------------------------------------------------
//
// The mouth is a small friendly closed smile, and it is EXACTLY THE SAME in
// every state. It never opens, stretches, deforms or changes thickness --
// an animated open mouth read as a frightened grimace on the real device.
// Only its colour changes, and only for the error face.
//
// Speech is shown by the audio bars below instead, which is why there is no
// mouth-opening parameter anywhere in this file.

// Depth of the smile bow and the thickness of its band, both as fractions of
// the mouth's base height. Constants, not animated inputs.
const float kMouthCurveRatio = 0.16f;
const float kSmileThicknessRatio = 0.11f;

// --- audio level bars -------------------------------------------------------

// Below this level the bars fade in colour as well as shrinking, so they
// disappear gradually into the silent smile.
const float kBarFadeBelow = 0.22f;

// Levels this quiet are treated as exact silence, so background noise cannot
// make the bars twitch.
const float kSpeechSilenceThreshold = 0.06f;

// Bars rise quickly and fall slowly. Exponential time constants, applied per
// millisecond so the result does not depend on the frame rate.
const float kSpeechAttackTauMs = 55.0f;
const float kSpeechReleaseTauMs = 190.0f;

// --- per-state eye scale ----------------------------------------------------
const float kEyeScaleReady = 1.00f;
const float kEyeScaleListening = 1.12f;  // attentive, slightly enlarged
const float kEyeScaleWaiting = 0.92f;    // narrowed, thoughtful
const float kEyeScaleSpeaking = 1.05f;
const float kEyeScaleError = 0.90f;
const float kEyeScaleSleeping = 0.92f;

// --- sleeping (offline / connecting, PHASE6_PLAN §9) -------------------------
//
// Drowsy rather than switched off: eyes about a third open, in dim cyan, with
// a slow blink that closes from there.
const float kSleepingOpenness = 0.35f;
const uint32_t kSleepBlinkCloseMs = 450;
const uint32_t kSleepBlinkOpenMs = 450;

// The largest eye scale any state uses. The eye sprite box is sized from this.
const float kEyeScaleMax = kEyeScaleListening;

}  // namespace face

// Attack/release envelope follower for the audio bar level.
//
// Portable and clock-injected like everything else here, so the smoothing
// curve, the silence clamp and the settling behaviour are all testable
// directly.
class SpeechLevelSmoother {
 public:
  SpeechLevelSmoother();

  // `target` is the raw 0..1 amplitude; `dtMs` the time since the last call.
  // Returns the smoothed level, 0..1.
  float update(float target, uint32_t dtMs);

  float value() const { return _value; }
  void reset(float value);

 private:
  float _value;
};

class FaceAnimator {
 public:
  explicit FaceAnimator(uint32_t seed);

  // Switches expression. Resets whatever animation the previous state was
  // running so a state change is visible immediately -- the LISTENING face
  // must appear the instant the button is pressed, not at the end of a blink.
  void setState(FaceState next, uint32_t nowMs);
  FaceState state() const { return _state; }

  // `speechLevel` is the already-smoothed bar level (see SpeechLevelSmoother),
  // used only while speaking. Every other state reports silence.
  FaceFrame update(uint32_t nowMs, float speechLevel);

  // --- test hooks ---
  bool isBlinking() const { return _blinking; }
  uint32_t nextBlinkAtMs() const { return _nextBlinkAtMs; }
  static bool blinksInState(FaceState state);

 private:
  uint32_t nextRandom();
  uint32_t randomInRange(uint32_t minInclusive, uint32_t maxInclusive);
  void scheduleBlink(uint32_t nowMs, bool quick);
  void scheduleDrift(uint32_t nowMs);
  float updateBlink(uint32_t nowMs);
  void updateDrift(uint32_t nowMs);

  FaceState _state;
  uint32_t _seed;
  uint32_t _stateEnteredMs;

  bool _blinking;
  uint32_t _blinkStartedMs;
  uint32_t _nextBlinkAtMs;
  bool _pendingDoubleBlink;

  uint32_t _driftStartedMs;
  uint32_t _nextDriftAtMs;
  float _driftFromX, _driftFromY;
  float _driftToX, _driftToY;
};

// Deterministic speech amplitude for Phase 3, where no audio playback exists
// yet.
//
// Walks a repeating 12 second cycle of three phrases -- quiet, medium, then
// loud -- separated by real silence, so the bars demonstrably show all three
// loudness levels and visibly settle to nothing in between. Feed it through
// SpeechLevelSmoother. Always within 0..1.
float simulatedSpeechAmplitude(uint32_t elapsedMs);

}  // namespace tth
