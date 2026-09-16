#include "tth/FaceAnimator.h"

#include <math.h>

namespace tth {

namespace {

const float kPi = 3.14159265358979323846f;

float clamp01(float v) {
  if (v < 0.0f) return 0.0f;
  if (v > 1.0f) return 1.0f;
  return v;
}

// Flutter uses Curves.easeInOutSine for the idle pupil move.
float easeInOutSine(float t) {
  return -(cosf(kPi * clamp01(t)) - 1.0f) * 0.5f;
}

float lerp(float a, float b, float t) { return a + (b - a) * t; }

}  // namespace

// ---------------------------------------------------------------------------
// SpeechLevelSmoother
// ---------------------------------------------------------------------------

SpeechLevelSmoother::SpeechLevelSmoother() : _value(0.0f) {}

void SpeechLevelSmoother::reset(float value) { _value = clamp01(value); }

float SpeechLevelSmoother::update(float target, uint32_t dtMs) {
  float wanted = clamp01(target);

  // Anything this quiet is silence. Without the clamp the bars twitch
  // continuously on near-zero amplitudes, which is exactly the rapid chatter
  // that looks wrong.
  if (wanted < face::kSpeechSilenceThreshold) wanted = 0.0f;

  // Rising is quick, falling is slow -- that asymmetry is most of what makes
  // the bars read as speech rather than as a meter.
  const float tau = (wanted > _value) ? face::kSpeechAttackTauMs
                                      : face::kSpeechReleaseTauMs;

  // Frame-rate independent exponential approach.
  const float alpha = 1.0f - expf(-static_cast<float>(dtMs) / tau);
  _value += alpha * (wanted - _value);

  // Stop infinitesimal tails from keeping the lower face marked dirty forever.
  if (_value < 0.001f) _value = 0.0f;

  return clamp01(_value);
}

// ---------------------------------------------------------------------------
// FaceAnimator
// ---------------------------------------------------------------------------

FaceAnimator::FaceAnimator(uint32_t seed)
    : _state(FaceState::Ready),
      // A zero seed would leave the generator stuck at zero forever.
      _seed(seed == 0 ? 0x9E3779B9u : seed),
      _stateEnteredMs(0),
      _blinking(false),
      _blinkStartedMs(0),
      _nextBlinkAtMs(0),
      _pendingDoubleBlink(false),
      _driftStartedMs(0),
      _nextDriftAtMs(0),
      _driftFromX(0.0f),
      _driftFromY(0.0f),
      _driftToX(0.0f),
      _driftToY(0.0f) {
  scheduleBlink(0, false);
  scheduleDrift(0);
}

// Numerical Recipes linear congruential generator: tiny, deterministic and
// perfectly adequate for "when should the next blink be".
uint32_t FaceAnimator::nextRandom() {
  _seed = (1664525u * _seed) + 1013904223u;
  return _seed;
}

uint32_t FaceAnimator::randomInRange(uint32_t minInclusive,
                                     uint32_t maxInclusive) {
  const uint32_t span = maxInclusive - minInclusive + 1u;
  return minInclusive + (nextRandom() % span);
}

bool FaceAnimator::blinksInState(FaceState state) {
  // Matches RobotFace._canBlink: idle and attentive faces blink; a face that
  // is thinking, talking or in error holds still.
  // Sleeping blinks too -- slowly, from half-closed eyes -- so an offline
  // robot still looks alive.
  return state == FaceState::Ready || state == FaceState::Listening ||
         state == FaceState::Sleeping;
}

void FaceAnimator::scheduleBlink(uint32_t nowMs, bool quick) {
  _blinking = false;
  _nextBlinkAtMs =
      nowMs + (quick ? face::kDoubleBlinkGapMs
                     : randomInRange(face::kBlinkMinIntervalMs,
                                     face::kBlinkMaxIntervalMs));
}

void FaceAnimator::scheduleDrift(uint32_t nowMs) {
  _nextDriftAtMs = nowMs + randomInRange(face::kDriftMinIntervalMs,
                                         face::kDriftMaxIntervalMs);
}

void FaceAnimator::setState(FaceState next, uint32_t nowMs) {
  if (_state == next) return;
  _state = next;
  _stateEnteredMs = nowMs;

  // Abandon any in-flight blink so the new expression is visible immediately.
  _blinking = false;
  _pendingDoubleBlink = false;
  scheduleBlink(nowMs, false);

  // The gaze keeps its current position and drifts on from there; snapping the
  // pupils back to centre on every state change would look mechanical.
  _driftFromX = _driftToX;
  _driftFromY = _driftToY;
  _driftStartedMs = nowMs;
  scheduleDrift(nowMs);
}

float FaceAnimator::updateBlink(uint32_t nowMs) {
  if (!blinksInState(_state)) {
    _blinking = false;
    return 1.0f;
  }

  if (!_blinking) {
    if (static_cast<int32_t>(nowMs - _nextBlinkAtMs) < 0) return 1.0f;
    _blinking = true;
    _blinkStartedMs = nowMs;
  }

  const uint32_t elapsed = nowMs - _blinkStartedMs;
  const bool sleeping = (_state == FaceState::Sleeping);
  const uint32_t closeMs =
      sleeping ? face::kSleepBlinkCloseMs : face::kBlinkCloseMs;
  const uint32_t openMs = sleeping ? face::kSleepBlinkOpenMs : face::kBlinkOpenMs;

  if (elapsed < closeMs) {
    return 1.0f - (static_cast<float>(elapsed) / static_cast<float>(closeMs));
  }

  if (elapsed < closeMs + openMs) {
    const uint32_t openElapsed = elapsed - closeMs;
    return static_cast<float>(openElapsed) / static_cast<float>(openMs);
  }

  // Blink finished. Occasionally follow it immediately with a second one.
  if (_pendingDoubleBlink) {
    _pendingDoubleBlink = false;
    scheduleBlink(nowMs, false);
  } else {
    const bool doubleBlink =
        (nextRandom() % 100u) < static_cast<uint32_t>(face::kDoubleBlinkPercent);
    _pendingDoubleBlink = doubleBlink;
    scheduleBlink(nowMs, doubleBlink);
  }
  return 1.0f;
}

void FaceAnimator::updateDrift(uint32_t nowMs) {
  if (static_cast<int32_t>(nowMs - _nextDriftAtMs) < 0) return;

  _driftFromX = _driftToX;
  _driftFromY = _driftToY;

  const float unitX =
      (static_cast<float>(nextRandom() % 2001u) / 1000.0f) - 1.0f;
  const float unitY =
      (static_cast<float>(nextRandom() % 2001u) / 1000.0f) - 1.0f;
  _driftToX = unitX * face::kDriftMaxX;
  _driftToY = unitY * face::kDriftMaxY;

  _driftStartedMs = nowMs;
  scheduleDrift(nowMs);
}

FaceFrame FaceAnimator::update(uint32_t nowMs, float speechLevel) {
  FaceFrame frame;
  frame.dim = (_state == FaceState::Error || _state == FaceState::Sleeping);
  frame.eyeOpenness = updateBlink(nowMs);

  // The smile is identical in every state, so only SPEAKING has anything to
  // show below the eyes. Every other state is silent by definition.
  frame.speechLevel = 0.0f;

  switch (_state) {
    case FaceState::Ready:
      frame.eyeScale = face::kEyeScaleReady;
      break;

    case FaceState::Listening:
      frame.eyeScale = face::kEyeScaleListening;
      break;

    case FaceState::Waiting:
      frame.eyeScale = face::kEyeScaleWaiting;
      break;

    case FaceState::Speaking:
      frame.eyeScale = face::kEyeScaleSpeaking;
      frame.speechLevel = clamp01(speechLevel);
      break;

    case FaceState::Sleeping:
      frame.eyeScale = face::kEyeScaleSleeping;
      // The slow blink scales down from the drowsy opening.
      frame.eyeOpenness = face::kSleepingOpenness * frame.eyeOpenness;
      break;

    case FaceState::Error:
      frame.eyeScale = face::kEyeScaleError;
      // Half-lidded rather than wide: clearly different, but calm rather than
      // alarming.
      frame.eyeOpenness = 0.55f;
      break;
  }

  if (_state == FaceState::Waiting) {
    // Deterministic slow sweep instead of random drift: this reads as
    // thinking, and it is the "subtle non-blocking waiting animation".
    const float phase = static_cast<float>(nowMs - _stateEnteredMs) /
                        static_cast<float>(face::kThinkingSweepPeriodMs);
    frame.pupilDx = face::kThinkingSweepX * sinf(2.0f * kPi * phase);
    frame.pupilDy = face::kThinkingLookUpY;
  } else if (_state == FaceState::Error || _state == FaceState::Sleeping) {
    frame.pupilDx = 0.0f;
    frame.pupilDy = 0.0f;
  } else {
    updateDrift(nowMs);
    const float t = static_cast<float>(nowMs - _driftStartedMs) /
                    static_cast<float>(face::kDriftDurationMs);
    const float eased = easeInOutSine(t);
    frame.pupilDx = lerp(_driftFromX, _driftToX, eased);
    frame.pupilDy = lerp(_driftFromY, _driftToY, eased);
  }

  return frame;
}

float simulatedSpeechAmplitude(uint32_t elapsedMs) {
  // Three phrases of four seconds each: quiet, then medium, then loud, so a
  // single 12 second cycle demonstrates the whole range of the bars. Each
  // phrase speaks for 3.2 s and is followed by 0.8 s of real silence, which is
  // what lets the "bars disappear, only the smile remains" behaviour be seen.
  const float kPhraseSeconds = 4.0f;
  const float kSpeakingSeconds = 3.2f;
  const float kEdgeSeconds = 0.25f;
  const float kPhrasePeak[3] = {0.30f, 0.62f, 1.00f};

  const float t = static_cast<float>(elapsedMs) / 1000.0f;
  const float cycle = fmodf(t, kPhraseSeconds * 3.0f);
  const int phrase = static_cast<int>(cycle / kPhraseSeconds);
  const float within = fmodf(cycle, kPhraseSeconds);

  if (within >= kSpeakingSeconds) return 0.0f;

  // A calm syllable rhythm. Never drops to zero mid-phrase, so the bars pulse
  // rather than flicker on and off.
  const float syllable = 0.5f + 0.5f * sinf(2.0f * kPi * 2.4f * within);
  const float shaped = 0.55f + (0.45f * syllable);

  // Ease in and out at the phrase edges so a phrase never starts or stops
  // abruptly.
  float edge = 1.0f;
  if (within < kEdgeSeconds) {
    edge = within / kEdgeSeconds;
  } else if (within > kSpeakingSeconds - kEdgeSeconds) {
    edge = (kSpeakingSeconds - within) / kEdgeSeconds;
  }
  edge = clamp01(edge);
  // Smoothstep, so the edges have no corner.
  edge = edge * edge * (3.0f - (2.0f * edge));

  return clamp01(kPhrasePeak[phrase] * shaped * edge);
}

}  // namespace tth
