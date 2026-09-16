// Host-side tests for the face's behaviour: blink timing, pupil drift,
// per-state expression, the simulated speaking envelope and the diagnostic
// override.

#include <unity.h>

#include "tth/FaceAnimator.h"
#include "tth/FaceOverride.h"
#include "tth/FaceState.h"

namespace {

const uint32_t kSeed = 0x54544842u;

// Runs the animator forward one millisecond at a time, reporting how many
// distinct blinks were observed and the interval bounds between them.
struct BlinkObservation {
  int blinks;
  uint32_t minInterval;
  uint32_t maxInterval;
  float minOpenness;
};

BlinkObservation observeBlinks(tth::FaceState state, uint32_t durationMs) {
  tth::FaceAnimator animator(kSeed);
  animator.setState(state, 0);

  BlinkObservation obs;
  obs.blinks = 0;
  obs.minInterval = 0xFFFFFFFFu;
  obs.maxInterval = 0;
  obs.minOpenness = 1.0f;

  bool wasBlinking = false;
  uint32_t lastBlinkStart = 0;

  for (uint32_t t = 1; t <= durationMs; ++t) {
    const tth::FaceFrame frame = animator.update(t, 0.0f);
    if (frame.eyeOpenness < obs.minOpenness) obs.minOpenness = frame.eyeOpenness;

    const bool blinking = frame.eyeOpenness < 0.99f;
    if (blinking && !wasBlinking) {
      ++obs.blinks;
      if (obs.blinks > 1) {
        const uint32_t interval = t - lastBlinkStart;
        if (interval < obs.minInterval) obs.minInterval = interval;
        if (interval > obs.maxInterval) obs.maxInterval = interval;
      }
      lastBlinkStart = t;
    }
    wasBlinking = blinking;
  }
  return obs;
}

}  // namespace

void setUp() {}
void tearDown() {}

// --- blink -----------------------------------------------------------------

static void ready_blinks_within_the_two_and_a_half_to_six_second_band() {
  const BlinkObservation obs = observeBlinks(tth::FaceState::Ready, 60000);

  TEST_ASSERT_TRUE(obs.blinks >= 8);

  // Double-blinks are deliberately quicker than the normal band, so only the
  // upper bound applies to every interval.
  TEST_ASSERT_TRUE(obs.maxInterval <=
                   tth::face::kBlinkMaxIntervalMs + tth::face::kBlinkCloseMs +
                       tth::face::kBlinkOpenMs);
}

static void a_blink_fully_closes_the_eye() {
  const BlinkObservation obs = observeBlinks(tth::FaceState::Ready, 30000);
  TEST_ASSERT_TRUE(obs.minOpenness < 0.02f);
}

// Excluding the deliberate double-blink follow-ups, gaps must respect the
// documented minimum.
static void normal_blink_gaps_respect_the_minimum() {
  const BlinkObservation obs = observeBlinks(tth::FaceState::Ready, 60000);
  // The shortest possible gap is a double blink: close+open then the gap.
  const uint32_t shortestPossible = tth::face::kBlinkCloseMs +
                                    tth::face::kBlinkOpenMs +
                                    tth::face::kDoubleBlinkGapMs;
  TEST_ASSERT_TRUE(obs.minInterval >= shortestPossible);
}

static void listening_blinks_too() {
  const BlinkObservation obs = observeBlinks(tth::FaceState::Listening, 30000);
  TEST_ASSERT_TRUE(obs.blinks >= 3);
}

// A face that is thinking, talking or in error holds still.
static void waiting_speaking_and_error_never_blink() {
  const tth::FaceState still[3] = {tth::FaceState::Waiting,
                                   tth::FaceState::Speaking,
                                   tth::FaceState::Error};
  for (int i = 0; i < 3; ++i) {
    TEST_ASSERT_FALSE(tth::FaceAnimator::blinksInState(still[i]));
    tth::FaceAnimator animator(kSeed);
    animator.setState(still[i], 0);
    const float expected = animator.update(1, 0.0f).eyeOpenness;
    for (uint32_t t = 2; t <= 30000; t += 7) {
      TEST_ASSERT_EQUAL_FLOAT(expected, animator.update(t, 0.0f).eyeOpenness);
    }
  }
}

// --- expression per state --------------------------------------------------

static void listening_eyes_are_larger_than_ready_eyes() {
  tth::FaceAnimator animator(kSeed);
  animator.setState(tth::FaceState::Ready, 0);
  const float ready = animator.update(1, 0.0f).eyeScale;
  animator.setState(tth::FaceState::Listening, 10);
  const float listening = animator.update(11, 0.0f).eyeScale;
  TEST_ASSERT_TRUE(listening > ready);
}

static void waiting_eyes_are_narrower_than_ready_eyes() {
  tth::FaceAnimator animator(kSeed);
  animator.setState(tth::FaceState::Ready, 0);
  const float ready = animator.update(1, 0.0f).eyeScale;
  animator.setState(tth::FaceState::Waiting, 10);
  const float waiting = animator.update(11, 0.0f).eyeScale;
  TEST_ASSERT_TRUE(waiting < ready);
}

static void only_error_and_sleeping_are_dim() {
  const tth::FaceState all[6] = {tth::FaceState::Ready, tth::FaceState::Listening,
                                 tth::FaceState::Waiting,
                                 tth::FaceState::Speaking,
                                 tth::FaceState::Error,
                                 tth::FaceState::Sleeping};
  for (int i = 0; i < 6; ++i) {
    tth::FaceAnimator animator(kSeed);
    animator.setState(all[i], 0);
    const bool dim = animator.update(1, 0.0f).dim;
    TEST_ASSERT_EQUAL(all[i] == tth::FaceState::Error ||
                          all[i] == tth::FaceState::Sleeping,
                      dim);
  }
}

// A state change must show immediately, not at the end of the current blink.
static void a_state_change_takes_effect_at_once() {
  tth::FaceAnimator animator(kSeed);
  animator.setState(tth::FaceState::Ready, 0);

  // Run until a blink is in progress.
  uint32_t t = 1;
  while (t < 20000 && animator.update(t, 0.0f).eyeOpenness > 0.5f) ++t;
  TEST_ASSERT_TRUE(t < 20000);

  animator.setState(tth::FaceState::Listening, t);
  const tth::FaceFrame frame = animator.update(t, 0.0f);
  TEST_ASSERT_EQUAL_FLOAT(tth::face::kEyeScaleListening, frame.eyeScale);
  TEST_ASSERT_TRUE(frame.eyeOpenness > 0.99f);
}

// --- pupil movement --------------------------------------------------------

static void idle_pupil_drift_stays_small_and_actually_moves() {
  tth::FaceAnimator animator(kSeed);
  animator.setState(tth::FaceState::Ready, 0);

  float maxAbsX = 0.0f;
  float maxAbsY = 0.0f;
  bool moved = false;
  for (uint32_t t = 1; t <= 60000; t += 3) {
    const tth::FaceFrame frame = animator.update(t, 0.0f);
    const float ax = frame.pupilDx < 0 ? -frame.pupilDx : frame.pupilDx;
    const float ay = frame.pupilDy < 0 ? -frame.pupilDy : frame.pupilDy;
    if (ax > maxAbsX) maxAbsX = ax;
    if (ay > maxAbsY) maxAbsY = ay;
    if (ax > 0.005f) moved = true;
  }

  TEST_ASSERT_TRUE(moved);
  TEST_ASSERT_TRUE(maxAbsX <= tth::face::kDriftMaxX + 0.001f);
  TEST_ASSERT_TRUE(maxAbsY <= tth::face::kDriftMaxY + 0.001f);
}

// Waiting sweeps deterministically instead of drifting randomly, and looks
// slightly upward the whole time.
static void waiting_sweeps_the_gaze_and_looks_up() {
  tth::FaceAnimator animator(kSeed);
  animator.setState(tth::FaceState::Waiting, 0);

  float minX = 1.0f;
  float maxX = -1.0f;
  for (uint32_t t = 1; t <= tth::face::kThinkingSweepPeriodMs; t += 5) {
    const tth::FaceFrame frame = animator.update(t, 0.0f);
    if (frame.pupilDx < minX) minX = frame.pupilDx;
    if (frame.pupilDx > maxX) maxX = frame.pupilDx;
    TEST_ASSERT_EQUAL_FLOAT(tth::face::kThinkingLookUpY, frame.pupilDy);
  }

  // A full sweep to both sides within one period.
  TEST_ASSERT_TRUE(maxX > tth::face::kThinkingSweepX * 0.9f);
  TEST_ASSERT_TRUE(minX < -tth::face::kThinkingSweepX * 0.9f);
}

// --- diagnostic override ---------------------------------------------------

static void the_override_is_inactive_until_a_key_arrives() {
  tth::FaceOverride override(2500);
  TEST_ASSERT_FALSE(override.isActive());
  TEST_ASSERT_EQUAL(tth::FaceState::Waiting,
                    override.faceFor(tth::FaceState::Waiting, 0));
}

static void each_key_selects_its_face() {
  const char keys[5] = {'r', 'l', 'w', 's', 'e'};
  const tth::FaceState expected[5] = {
      tth::FaceState::Ready, tth::FaceState::Listening, tth::FaceState::Waiting,
      tth::FaceState::Speaking, tth::FaceState::Error};

  for (int i = 0; i < 5; ++i) {
    tth::FaceOverride override(2500);
    TEST_ASSERT_TRUE(override.handleKey(keys[i], 0));
    TEST_ASSERT_TRUE(override.isActive());
    // Whatever the production face is, the override wins.
    TEST_ASSERT_EQUAL(expected[i],
                      override.faceFor(tth::FaceState::Ready, 100));
  }
}

static void unknown_keys_are_rejected_and_change_nothing() {
  tth::FaceOverride override(2500);
  TEST_ASSERT_FALSE(override.handleKey('z', 0));
  TEST_ASSERT_FALSE(override.handleKey('\n', 0));
  TEST_ASSERT_FALSE(override.isActive());
}

static void cycle_mode_visits_every_face_in_order() {
  tth::FaceOverride override(1000);
  TEST_ASSERT_TRUE(override.handleKey('a', 0));
  TEST_ASSERT_TRUE(override.isCycling());

  for (int i = 0; i < tth::kFaceCycleLength; ++i) {
    const uint32_t t = static_cast<uint32_t>(i) * 1000u + 500u;
    TEST_ASSERT_EQUAL(tth::kFaceCycle[i],
                      override.faceFor(tth::FaceState::Ready, t));
  }
  // ...and wraps.
  TEST_ASSERT_EQUAL(tth::kFaceCycle[0],
                    override.faceFor(tth::FaceState::Ready,
                                     static_cast<uint32_t>(tth::kFaceCycleLength) *
                                             1000u +
                                         500u));
}

static void the_cycle_covers_every_face_exactly_once() {
  bool seen[tth::kFaceCycleLength] = {false, false, false, false, false, false};
  for (int i = 0; i < tth::kFaceCycleLength; ++i) {
    const int index = static_cast<int>(tth::kFaceCycle[i]);
    TEST_ASSERT_TRUE(index >= 0 && index < tth::kFaceCycleLength);
    TEST_ASSERT_FALSE(seen[index]);
    seen[index] = true;
  }
}

static void clearing_returns_the_face_to_the_state_machine() {
  tth::FaceOverride override(2500);
  override.handleKey('e', 0);
  TEST_ASSERT_EQUAL(tth::FaceState::Error,
                    override.faceFor(tth::FaceState::Listening, 10));

  override.clear();
  TEST_ASSERT_FALSE(override.isActive());
  TEST_ASSERT_EQUAL(tth::FaceState::Listening,
                    override.faceFor(tth::FaceState::Listening, 20));
}

// --- Step 6.1: the Sleeping face ----------------------------------------------

static void sleeping_is_dim_drowsy_still_and_silent() {
  tth::FaceAnimator animator(1);
  animator.setState(tth::FaceState::Sleeping, 0);
  for (uint32_t t = 0; t < 20000; t += 7) {
    const tth::FaceFrame frame = animator.update(t, 1.0f);
    TEST_ASSERT_TRUE(frame.dim);
    TEST_ASSERT_TRUE(frame.eyeOpenness >= 0.0f);
    TEST_ASSERT_TRUE(frame.eyeOpenness <= tth::face::kSleepingOpenness + 1e-5f);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, frame.speechLevel);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, frame.pupilDx);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, frame.pupilDy);
    TEST_ASSERT_EQUAL_FLOAT(tth::face::kEyeScaleSleeping, frame.eyeScale);
  }
}

static void sleeping_blinks_slowly() {
  TEST_ASSERT_TRUE(tth::face::kSleepBlinkCloseMs > tth::face::kBlinkCloseMs);
  tth::FaceAnimator animator(7);
  animator.setState(tth::FaceState::Sleeping, 0);
  uint32_t start = 0;
  bool found = false;
  for (uint32_t t = 0; t < 10000; ++t) {
    if (animator.update(t, 0.0f).eyeOpenness < tth::face::kSleepingOpenness) {
      start = t;
      found = true;
      break;
    }
  }
  TEST_ASSERT_TRUE(found);
  // Half-way through the close the eyes are still about half as open.
  const float mid =
      animator.update(start + tth::face::kSleepBlinkCloseMs / 2u, 0.0f).eyeOpenness;
  TEST_ASSERT_TRUE(mid > 0.10f && mid < 0.25f);
}

static void the_o_key_holds_the_sleeping_face() {
  tth::FaceOverride override(2500);
  TEST_ASSERT_TRUE(override.handleKey('o', 0));
  TEST_ASSERT_EQUAL(tth::FaceState::Sleeping,
                    override.faceFor(tth::FaceState::Ready, 10));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(sleeping_is_dim_drowsy_still_and_silent);
  RUN_TEST(sleeping_blinks_slowly);
  RUN_TEST(the_o_key_holds_the_sleeping_face);
  RUN_TEST(ready_blinks_within_the_two_and_a_half_to_six_second_band);
  RUN_TEST(a_blink_fully_closes_the_eye);
  RUN_TEST(normal_blink_gaps_respect_the_minimum);
  RUN_TEST(listening_blinks_too);
  RUN_TEST(waiting_speaking_and_error_never_blink);
  RUN_TEST(listening_eyes_are_larger_than_ready_eyes);
  RUN_TEST(waiting_eyes_are_narrower_than_ready_eyes);
  RUN_TEST(only_error_and_sleeping_are_dim);
  RUN_TEST(a_state_change_takes_effect_at_once);
  RUN_TEST(idle_pupil_drift_stays_small_and_actually_moves);
  RUN_TEST(waiting_sweeps_the_gaze_and_looks_up);
  RUN_TEST(the_override_is_inactive_until_a_key_arrives);
  RUN_TEST(each_key_selects_its_face);
  RUN_TEST(unknown_keys_are_rejected_and_change_nothing);
  RUN_TEST(cycle_mode_visits_every_face_in_order);
  RUN_TEST(the_cycle_covers_every_face_exactly_once);
  RUN_TEST(clearing_returns_the_face_to_the_state_machine);
  return UNITY_END();
}
