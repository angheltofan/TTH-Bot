// Host-side tests for the lower face: the FIXED smile and the audio level
// bars that replaced the animated open mouth.
//
// The central guarantee is that the smile is identical in every state. An
// animated open mouth read as a frightened grimace on the real device, so
// nothing here may reintroduce one.

#include <unity.h>

#include "tth/FaceAnimator.h"
#include "tth/FaceFrame.h"
#include "tth/FaceGeometry.h"
#include "tth/FaceOverride.h"

namespace {

const uint32_t kFrameMs = 40;  // 25 fps, the rate the face actually runs at
const uint32_t kSeed = 0x54544842u;

tth::FaceGeometry core2() { return tth::FaceGeometry::forScreen(320, 240); }

tth::FaceFrame frameAtLevel(float level) {
  tth::FaceFrame f;
  f.eyeScale = 1.0f;
  f.eyeOpenness = 1.0f;
  f.pupilDx = 0.0f;
  f.pupilDy = 0.0f;
  f.speechLevel = level;
  f.dim = false;
  return f;
}

float settleAt(float amplitude, uint32_t durationMs) {
  tth::SpeechLevelSmoother smoother;
  float value = 0.0f;
  for (uint32_t t = 0; t < durationMs; t += kFrameMs) {
    value = smoother.update(amplitude, kFrameMs);
  }
  return value;
}

}  // namespace

void setUp() {}
void tearDown() {}

// --- the smile never changes ------------------------------------------------

// The FaceFrame carries no mouth-opening parameter at all, so the smile is
// drawn from constants alone. This asserts the states agree on everything the
// lower face could possibly key off, other than the bars and the dim colour.
static void the_smile_is_identical_in_ready_and_speaking() {
  tth::FaceAnimator ready(kSeed);
  ready.setState(tth::FaceState::Ready, 0);
  const tth::FaceFrame readyFrame = ready.update(10, 0.0f);

  tth::FaceAnimator speaking(kSeed);
  speaking.setState(tth::FaceState::Speaking, 0);
  const tth::FaceFrame speakingFrame = speaking.update(10, 1.0f);

  // Same colour treatment...
  TEST_ASSERT_EQUAL(readyFrame.dim, speakingFrame.dim);
  // ...and the only difference below the eyes is the bar level.
  TEST_ASSERT_EQUAL_FLOAT(0.0f, readyFrame.speechLevel);
  TEST_ASSERT_EQUAL_FLOAT(1.0f, speakingFrame.speechLevel);
}

// Every state must report a silent lower face except Speaking, so the smile
// stands alone and nothing else can animate it.
static void only_speaking_ever_shows_bars() {
  const tth::FaceState quiet[4] = {
      tth::FaceState::Ready, tth::FaceState::Listening, tth::FaceState::Waiting,
      tth::FaceState::Error};

  for (int i = 0; i < 4; ++i) {
    tth::FaceAnimator animator(kSeed);
    animator.setState(quiet[i], 0);
    // Even handed a full-scale level, a non-speaking face reports silence.
    TEST_ASSERT_EQUAL_FLOAT(0.0f, animator.update(10, 1.0f).speechLevel);
  }

  tth::FaceAnimator speaking(kSeed);
  speaking.setState(tth::FaceState::Speaking, 0);
  TEST_ASSERT_EQUAL_FLOAT(1.0f, speaking.update(10, 1.0f).speechLevel);
}

// The smile occupies the same pixels regardless of level: its geometry is
// derived only from the mouth centre, width and height, none of which the
// frame can influence.
static void the_smile_geometry_does_not_depend_on_the_level() {
  const tth::FaceGeometry g = core2();
  const float widthBefore = g.mouthWidth;
  const float heightBefore = g.mouthHeight;
  const int centreX = g.mouthCenterX;
  const int centreY = g.mouthCenterY;

  // Resolving bars at every level must not disturb the smile's inputs.
  tth::BarRender bars[tth::kBarCount];
  for (int step = 0; step <= 10; ++step) {
    tth::audioBarsFor(g, frameAtLevel(static_cast<float>(step) / 10.0f), bars);
  }

  TEST_ASSERT_EQUAL_FLOAT(widthBefore, g.mouthWidth);
  TEST_ASSERT_EQUAL_FLOAT(heightBefore, g.mouthHeight);
  TEST_ASSERT_EQUAL_INT(centreX, g.mouthCenterX);
  TEST_ASSERT_EQUAL_INT(centreY, g.mouthCenterY);
}

// --- bar layout -------------------------------------------------------------

static void there_are_three_bars_on_each_side() {
  TEST_ASSERT_EQUAL_INT(3, tth::kBarsPerSide);
  TEST_ASSERT_EQUAL_INT(6, tth::kBarCount);
}

// Bars i and (kBarCount-1-i) must mirror about the mouth centre exactly.
static void the_bars_are_symmetric_about_the_mouth_centre() {
  const tth::FaceGeometry g = core2();

  for (int i = 0; i < tth::kBarsPerSide; ++i) {
    const int left = g.barCenterX[i];
    const int right = g.barCenterX[tth::kBarCount - 1 - i];
    const int leftDistance = g.mouthCenterX - left;
    const int rightDistance = right - g.mouthCenterX;
    TEST_ASSERT_EQUAL_INT(leftDistance, rightDistance);

    // Mirrored bars must also be the same height.
    TEST_ASSERT_EQUAL_INT(g.barMaxHalfHeight[i],
                          g.barMaxHalfHeight[tth::kBarCount - 1 - i]);
  }
}

static void the_bars_render_symmetrically_at_every_level() {
  const tth::FaceGeometry g = core2();
  tth::BarRender bars[tth::kBarCount];

  for (int step = 0; step <= 20; ++step) {
    tth::audioBarsFor(g, frameAtLevel(static_cast<float>(step) / 20.0f), bars);
    for (int i = 0; i < tth::kBarsPerSide; ++i) {
      const tth::BarRender& left = bars[i];
      const tth::BarRender& right = bars[tth::kBarCount - 1 - i];
      TEST_ASSERT_EQUAL_INT(left.halfHeight, right.halfHeight);
      TEST_ASSERT_EQUAL_INT(left.halfWidth, right.halfWidth);
      TEST_ASSERT_EQUAL_INT(left.centerY, right.centerY);
      TEST_ASSERT_EQUAL(left.visible, right.visible);
      TEST_ASSERT_EQUAL_FLOAT(left.intensity, right.intensity);
    }
  }
}

// Inner bars are taller than outer bars, which is what gives the group its
// shape rather than looking like a fence.
static void inner_bars_are_taller_than_outer_bars() {
  const tth::FaceGeometry g = core2();
  TEST_ASSERT_TRUE(g.barMaxHalfHeight[2] > g.barMaxHalfHeight[1]);
  TEST_ASSERT_TRUE(g.barMaxHalfHeight[1] > g.barMaxHalfHeight[0]);
}

// Fixed horizontal positions: the bars must not move with the amplitude.
static void bar_positions_do_not_move_with_the_level() {
  const tth::FaceGeometry g = core2();
  tth::BarRender atZero[tth::kBarCount];
  tth::BarRender atFull[tth::kBarCount];

  tth::audioBarsFor(g, frameAtLevel(0.0f), atZero);
  tth::audioBarsFor(g, frameAtLevel(1.0f), atFull);

  for (int i = 0; i < tth::kBarCount; ++i) {
    TEST_ASSERT_EQUAL_INT(atZero[i].centerX, atFull[i].centerX);
    TEST_ASSERT_EQUAL_INT(atZero[i].centerY, atFull[i].centerY);
    TEST_ASSERT_EQUAL_INT(atZero[i].halfWidth, atFull[i].halfWidth);
  }
}

// --- bars must not touch the smile or leave the region ----------------------

static void no_bar_overlaps_the_smile() {
  const tth::FaceGeometry g = core2();

  for (int i = 0; i < tth::kBarCount; ++i) {
    const float barLeft = static_cast<float>(g.barCenterX[i] - g.barHalfWidth);
    const float barRight = static_cast<float>(g.barCenterX[i] + g.barHalfWidth);
    const bool leftOfSmile = barRight < g.smileLeftEdge();
    const bool rightOfSmile = barLeft > g.smileRightEdge();
    TEST_ASSERT_TRUE(leftOfSmile || rightOfSmile);
  }
}

// ...and there is a real visible gap, not merely no overlap.
static void every_bar_is_clear_of_the_smile_by_a_visible_margin() {
  const tth::FaceGeometry g = core2();

  for (int i = 0; i < tth::kBarCount; ++i) {
    const float barLeft = static_cast<float>(g.barCenterX[i] - g.barHalfWidth);
    const float barRight = static_cast<float>(g.barCenterX[i] + g.barHalfWidth);
    const float gap = (barRight < g.smileLeftEdge())
                          ? (g.smileLeftEdge() - barRight)
                          : (barLeft - g.smileRightEdge());
    TEST_ASSERT_TRUE(gap >= 3.0f);
  }
}

static void every_bar_stays_inside_the_lower_face_sprite() {
  const tth::FaceGeometry g = core2();
  tth::BarRender bars[tth::kBarCount];
  // Full level is the worst case for height.
  tth::audioBarsFor(g, frameAtLevel(1.0f), bars);

  for (int i = 0; i < tth::kBarCount; ++i) {
    const tth::BarRender& bar = bars[i];
    TEST_ASSERT_TRUE(bar.centerX - bar.halfWidth >= 0);
    TEST_ASSERT_TRUE(bar.centerX + bar.halfWidth <= g.lowerBoxW);
    TEST_ASSERT_TRUE(bar.centerY - bar.halfHeight >= 0);
    TEST_ASSERT_TRUE(bar.centerY + bar.halfHeight <= g.lowerBoxH);
  }
}

// The bars must stay modest and well clear of the eyes.
static void the_tallest_bar_stays_well_below_the_eyes() {
  const tth::FaceGeometry g = core2();
  int tallest = 0;
  for (int i = 0; i < tth::kBarCount; ++i) {
    if (g.barMaxHalfHeight[i] > tallest) tallest = g.barMaxHalfHeight[i];
  }
  const int barTop = g.barCenterY - tallest;
  const int eyesBottom = g.eyesBoxY + g.eyesBoxH;

  TEST_ASSERT_TRUE(barTop > eyesBottom);
  // A clear visual separation, not a near miss.
  TEST_ASSERT_TRUE(barTop - eyesBottom >= 20);
}

static void the_tallest_bar_is_a_modest_fraction_of_the_mouth_height() {
  const tth::FaceGeometry g = core2();
  int tallest = 0;
  for (int i = 0; i < tth::kBarCount; ++i) {
    if (g.barMaxHalfHeight[i] > tallest) tallest = g.barMaxHalfHeight[i];
  }
  const float fullHeight = static_cast<float>(2 * tallest);
  TEST_ASSERT_TRUE(fullHeight <= g.mouthHeight);
}

// --- silence ----------------------------------------------------------------

static void silence_leaves_no_bars_at_all() {
  const tth::FaceGeometry g = core2();
  tth::BarRender bars[tth::kBarCount];
  tth::audioBarsFor(g, frameAtLevel(0.0f), bars);

  for (int i = 0; i < tth::kBarCount; ++i) {
    TEST_ASSERT_FALSE(bars[i].visible);
  }
}

// As the level falls the bars must shrink away and dim, not blink out.
static void the_bars_disappear_gradually_as_the_level_falls() {
  const tth::FaceGeometry g = core2();
  tth::BarRender bars[tth::kBarCount];

  int previousVisible = tth::kBarCount;
  float previousIntensity = 1.0f;

  for (int step = 20; step >= 0; --step) {
    const float level = static_cast<float>(step) / 20.0f;
    tth::audioBarsFor(g, frameAtLevel(level), bars);

    int visible = 0;
    for (int i = 0; i < tth::kBarCount; ++i) {
      if (bars[i].visible) ++visible;
    }
    // Never more bars as the level drops.
    TEST_ASSERT_TRUE(visible <= previousVisible);
    TEST_ASSERT_TRUE(bars[0].intensity <= previousIntensity + 0.0001f);
    previousVisible = visible;
    previousIntensity = bars[0].intensity;
  }
  TEST_ASSERT_EQUAL_INT(0, previousVisible);
}

static void low_levels_fade_the_bar_colour() {
  const tth::FaceGeometry g = core2();
  tth::BarRender bars[tth::kBarCount];

  tth::audioBarsFor(g, frameAtLevel(tth::face::kBarFadeBelow * 0.5f), bars);
  TEST_ASSERT_TRUE(bars[0].intensity < 1.0f);
  TEST_ASSERT_TRUE(bars[0].intensity > 0.0f);

  tth::audioBarsFor(g, frameAtLevel(1.0f), bars);
  TEST_ASSERT_EQUAL_FLOAT(1.0f, bars[0].intensity);
}

// --- smoothing --------------------------------------------------------------

static void the_level_never_leaves_the_zero_to_one_range() {
  tth::SpeechLevelSmoother smoother;
  const float inputs[6] = {-10.0f, -0.5f, 0.0f, 0.5f, 1.0f, 42.0f};
  for (int i = 0; i < 6; ++i) {
    for (int n = 0; n < 50; ++n) {
      const float v = smoother.update(inputs[i], kFrameMs);
      TEST_ASSERT_TRUE(v >= 0.0f);
      TEST_ASSERT_TRUE(v <= 1.0f);
    }
  }
}

static void a_full_amplitude_settles_at_full_level() {
  TEST_ASSERT_FLOAT_WITHIN(0.02f, 1.0f, settleAt(1.0f, 1500));
}

static void amplitudes_below_the_silence_threshold_settle_to_zero() {
  const float below = tth::face::kSpeechSilenceThreshold * 0.5f;
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, settleAt(below, 2500));
}

// Bars rise quickly and fall slowly.
static void rising_is_faster_than_falling() {
  tth::SpeechLevelSmoother rising;
  const float rose = rising.update(1.0f, kFrameMs);

  tth::SpeechLevelSmoother falling;
  falling.reset(1.0f);
  const float fell = 1.0f - falling.update(0.0f, kFrameMs);

  TEST_ASSERT_TRUE(rose > fell);
}

static void the_level_does_not_jump_in_a_single_frame() {
  tth::SpeechLevelSmoother smoother;
  const float first = smoother.update(1.0f, kFrameMs);
  TEST_ASSERT_TRUE(first < 0.95f);
  TEST_ASSERT_TRUE(first > 0.15f);
}

static void the_smoothing_is_frame_rate_independent() {
  tth::SpeechLevelSmoother coarse;
  float a = 0.0f;
  for (int i = 0; i < 5; ++i) a = coarse.update(1.0f, 40);

  tth::SpeechLevelSmoother fine;
  float b = 0.0f;
  for (int i = 0; i < 20; ++i) b = fine.update(1.0f, 10);

  TEST_ASSERT_FLOAT_WITHIN(0.02f, a, b);
}

static void a_silent_gap_settles_gradually_rather_than_snapping() {
  tth::SpeechLevelSmoother smoother;
  smoother.reset(1.0f);

  TEST_ASSERT_TRUE(smoother.update(0.0f, kFrameMs) > 0.5f);

  float value = 0.0f;
  for (uint32_t t = 0; t < 1500; t += kFrameMs) {
    value = smoother.update(0.0f, kFrameMs);
  }
  TEST_ASSERT_FLOAT_WITHIN(0.02f, 0.0f, value);
}

static void the_level_falls_monotonically_during_silence() {
  tth::SpeechLevelSmoother smoother;
  smoother.reset(1.0f);
  float previous = 1.0f;
  for (uint32_t t = 0; t < 1200; t += kFrameMs) {
    const float value = smoother.update(0.0f, kFrameMs);
    TEST_ASSERT_TRUE(value <= previous);
    previous = value;
  }
}

static void movement_between_frames_stays_smooth() {
  tth::SpeechLevelSmoother smoother;
  float previous = 0.0f;
  float biggestStep = 0.0f;

  for (uint32_t t = 0; t < 24000; t += kFrameMs) {
    const float value =
        smoother.update(tth::simulatedSpeechAmplitude(t), kFrameMs);
    const float step = value > previous ? value - previous : previous - value;
    if (step > biggestStep) biggestStep = step;
    previous = value;
  }
  TEST_ASSERT_TRUE(biggestStep < 0.30f);
}

// --- the simulated envelope -------------------------------------------------

static void the_envelope_stays_in_range() {
  for (uint32_t ms = 0; ms <= 40000; ms += 7) {
    const float a = tth::simulatedSpeechAmplitude(ms);
    TEST_ASSERT_TRUE(a >= 0.0f);
    TEST_ASSERT_TRUE(a <= 1.0f);
  }
}

// The whole point of the envelope is to demonstrate quiet, medium and loud
// speech, each separated by real silence.
static void the_envelope_demonstrates_quiet_medium_and_loud_phrases() {
  float peak[3] = {0.0f, 0.0f, 0.0f};
  bool silenceBetween[3] = {false, false, false};

  for (uint32_t ms = 0; ms < 12000; ms += 5) {
    const int phrase = static_cast<int>(ms / 4000u);
    const float a = tth::simulatedSpeechAmplitude(ms);
    if (a > peak[phrase]) peak[phrase] = a;
    if ((ms % 4000u) > 3400u && a <= 0.0f) silenceBetween[phrase] = true;
  }

  TEST_ASSERT_TRUE(peak[0] > 0.15f && peak[0] < 0.45f);   // quiet
  TEST_ASSERT_TRUE(peak[1] > 0.45f && peak[1] < 0.80f);   // medium
  TEST_ASSERT_TRUE(peak[2] > 0.80f);                      // loud
  TEST_ASSERT_TRUE(silenceBetween[0]);
  TEST_ASSERT_TRUE(silenceBetween[1]);
  TEST_ASSERT_TRUE(silenceBetween[2]);
}

static void the_envelope_is_deterministic() {
  for (uint32_t ms = 0; ms <= 5000; ms += 313) {
    TEST_ASSERT_EQUAL_FLOAT(tth::simulatedSpeechAmplitude(ms),
                            tth::simulatedSpeechAmplitude(ms));
  }
}

// --- the fixed-level diagnostic ---------------------------------------------

static void the_fixed_levels_are_the_documented_five() {
  TEST_ASSERT_EQUAL_INT(5, tth::kFixedLevelCount);
  TEST_ASSERT_EQUAL_FLOAT(0.00f, tth::fixedSpeechLevelFor(0));
  TEST_ASSERT_EQUAL_FLOAT(0.25f, tth::fixedSpeechLevelFor(1));
  TEST_ASSERT_EQUAL_FLOAT(0.50f, tth::fixedSpeechLevelFor(2));
  TEST_ASSERT_EQUAL_FLOAT(0.75f, tth::fixedSpeechLevelFor(3));
  TEST_ASSERT_EQUAL_FLOAT(1.00f, tth::fixedSpeechLevelFor(4));
}

// Past the loudest level the diagnostic hands control back to the envelope,
// signalled by a negative level.
static void stepping_wraps_through_the_levels_and_back_to_the_envelope() {
  int index = tth::kFixedLevelCount;  // start on the envelope
  TEST_ASSERT_TRUE(tth::fixedSpeechLevelFor(index) < 0.0f);

  for (int expected = 0; expected < tth::kFixedLevelCount; ++expected) {
    index = tth::nextFixedLevelIndex(index);
    TEST_ASSERT_EQUAL_FLOAT(tth::kFixedSpeechLevels[expected],
                            tth::fixedSpeechLevelFor(index));
  }

  index = tth::nextFixedLevelIndex(index);
  TEST_ASSERT_TRUE(tth::fixedSpeechLevelFor(index) < 0.0f);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(the_smile_is_identical_in_ready_and_speaking);
  RUN_TEST(only_speaking_ever_shows_bars);
  RUN_TEST(the_smile_geometry_does_not_depend_on_the_level);
  RUN_TEST(there_are_three_bars_on_each_side);
  RUN_TEST(the_bars_are_symmetric_about_the_mouth_centre);
  RUN_TEST(the_bars_render_symmetrically_at_every_level);
  RUN_TEST(inner_bars_are_taller_than_outer_bars);
  RUN_TEST(bar_positions_do_not_move_with_the_level);
  RUN_TEST(no_bar_overlaps_the_smile);
  RUN_TEST(every_bar_is_clear_of_the_smile_by_a_visible_margin);
  RUN_TEST(every_bar_stays_inside_the_lower_face_sprite);
  RUN_TEST(the_tallest_bar_stays_well_below_the_eyes);
  RUN_TEST(the_tallest_bar_is_a_modest_fraction_of_the_mouth_height);
  RUN_TEST(silence_leaves_no_bars_at_all);
  RUN_TEST(the_bars_disappear_gradually_as_the_level_falls);
  RUN_TEST(low_levels_fade_the_bar_colour);
  RUN_TEST(the_level_never_leaves_the_zero_to_one_range);
  RUN_TEST(a_full_amplitude_settles_at_full_level);
  RUN_TEST(amplitudes_below_the_silence_threshold_settle_to_zero);
  RUN_TEST(rising_is_faster_than_falling);
  RUN_TEST(the_level_does_not_jump_in_a_single_frame);
  RUN_TEST(the_smoothing_is_frame_rate_independent);
  RUN_TEST(a_silent_gap_settles_gradually_rather_than_snapping);
  RUN_TEST(the_level_falls_monotonically_during_silence);
  RUN_TEST(movement_between_frames_stays_smooth);
  RUN_TEST(the_envelope_stays_in_range);
  RUN_TEST(the_envelope_demonstrates_quiet_medium_and_loud_phrases);
  RUN_TEST(the_envelope_is_deterministic);
  RUN_TEST(the_fixed_levels_are_the_documented_five);
  RUN_TEST(stepping_wraps_through_the_levels_and_back_to_the_envelope);
  return UNITY_END();
}
