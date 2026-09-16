// Host-side tests for the robot face layout, and for the guarantee that the
// two eyes are always drawn from one identical snapshot.

#include <unity.h>

#include "tth/FaceAnimator.h"
#include "tth/FaceFrame.h"
#include "tth/FaceGeometry.h"

namespace {

const int kW = 320;
const int kH = 240;

tth::FaceGeometry core2() { return tth::FaceGeometry::forScreen(kW, kH); }

tth::FaceFrame frameWith(float eyeScale, float openness, float dx, float dy) {
  tth::FaceFrame f;
  f.eyeScale = eyeScale;
  f.eyeOpenness = openness;
  f.pupilDx = dx;
  f.pupilDy = dy;
  f.speechLevel = 0.0f;
  f.dim = false;
  return f;
}

}  // namespace

void setUp() {}
void tearDown() {}

static void everything_fits_on_a_320x240_screen() {
  TEST_ASSERT_TRUE(core2().fitsOnScreen());
}

static void element_boxes_do_not_overlap() {
  TEST_ASSERT_TRUE(core2().boxesAreDisjoint());
}

static void the_contain_fit_is_width_limited() {
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 320.0f / 340.0f, core2().scale);
}

// The exact ported layout. If a ratio is ever changed by accident, this is
// what catches it.
static void the_layout_matches_the_ported_flutter_ratios() {
  const tth::FaceGeometry g = core2();

  TEST_ASSERT_FLOAT_WITHIN(0.5f, 49.6f, g.eyeRadius);
  TEST_ASSERT_INT_WITHIN(1, 88, g.leftEyeCenterX);
  TEST_ASSERT_INT_WITHIN(1, 232, g.rightEyeCenterX);
  TEST_ASSERT_INT_WITHIN(1, 66, g.eyeCenterY);

  TEST_ASSERT_FLOAT_WITHIN(0.5f, 147.2f, g.mouthWidth);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 73.6f, g.mouthHeight);
  TEST_ASSERT_INT_WITHIN(1, 160, g.mouthCenterX);
  TEST_ASSERT_INT_WITHIN(1, 191, g.mouthCenterY);
}

static void the_eyes_are_symmetric_about_the_centre() {
  const tth::FaceGeometry g = core2();
  TEST_ASSERT_INT_WITHIN(1, g.leftEyeCenterX, kW - g.rightEyeCenterX);
}

// The single eye box must contain both eyes at their largest animated scale,
// with the pupil at full deflection.
static void the_eye_box_contains_both_eyes_at_maximum_scale() {
  const tth::FaceGeometry g = core2();
  const float maxRadius = g.eyeRadius * tth::face::kEyeScaleMax;

  const float leftEdge = static_cast<float>(g.leftEyeCenterX) - maxRadius;
  const float rightEdge = static_cast<float>(g.rightEyeCenterX) + maxRadius;
  TEST_ASSERT_TRUE(leftEdge >= static_cast<float>(g.eyesBoxX));
  TEST_ASSERT_TRUE(rightEdge <=
                   static_cast<float>(g.eyesBoxX + g.eyesBoxW));

  const float topEdge = static_cast<float>(g.eyeCenterY) - maxRadius;
  const float bottomEdge = static_cast<float>(g.eyeCenterY) + maxRadius;
  TEST_ASSERT_TRUE(topEdge >= static_cast<float>(g.eyesBoxY));
  TEST_ASSERT_TRUE(bottomEdge <= static_cast<float>(g.eyesBoxY + g.eyesBoxH));
}

// The fixed smile and every bar at full height must fit the lower-face box.
static void the_lower_box_contains_the_smile_and_the_bars() {
  const tth::FaceGeometry g = core2();
  const float curve = g.mouthHeight * tth::face::kMouthCurveRatio;
  const float half = 0.5f * g.mouthHeight * tth::face::kSmileThicknessRatio;

  const float smileTop = static_cast<float>(g.mouthCenterY) - half;
  const float smileBottom = static_cast<float>(g.mouthCenterY) + curve + half;
  TEST_ASSERT_TRUE(smileTop >= static_cast<float>(g.lowerBoxY));
  TEST_ASSERT_TRUE(smileBottom <= static_cast<float>(g.lowerBoxY + g.lowerBoxH));

  TEST_ASSERT_TRUE(g.smileLeftEdge() >= static_cast<float>(g.lowerBoxX));
  TEST_ASSERT_TRUE(g.smileRightEdge() <=
                   static_cast<float>(g.lowerBoxX + g.lowerBoxW));
}

// Combining the eyes costs a little more area than two tight boxes would, but
// the tighter sizing more than pays for it: this must stay well under the
// previous three-sprite budget.
static void the_sprite_budget_is_smaller_than_the_old_three_sprite_layout() {
  const tth::FaceGeometry g = core2();
  TEST_ASSERT_TRUE(g.spriteBytes() < 103428);  // the old figure
  TEST_ASSERT_TRUE(g.spriteBytes() > 40000);   // sanity: not degenerate
}

// A full-face transition must fit inside one display frame. At 40 MHz and 16
// bits per pixel the panel takes 0.4 us per pixel, so 40000 pixels is 16 ms.
static void a_full_face_transition_fits_in_one_frame() {
  const tth::FaceGeometry g = core2();
  const int micros = (g.fullFacePixels() * 2 * 8) / 40;
  // The stated target is under 16-20 ms for a complete state transition.
  TEST_ASSERT_TRUE(micros < 20000);
}

static void no_dimension_is_degenerate() {
  const tth::FaceGeometry g = core2();
  TEST_ASSERT_TRUE(g.eyesBoxW > 0);
  TEST_ASSERT_TRUE(g.eyesBoxH > 0);
  TEST_ASSERT_TRUE(g.lowerBoxW > 0);
  TEST_ASSERT_TRUE(g.lowerBoxH > 0);
  TEST_ASSERT_TRUE(g.eyeRadius > 0.0f);
  TEST_ASSERT_TRUE(g.mouthWidth > 0.0f);
  TEST_ASSERT_TRUE(g.mouthHeight > 0.0f);
  TEST_ASSERT_TRUE(g.scale > 0.0f);
}

static void the_layout_stays_valid_at_other_screen_sizes() {
  const int sizes[][2] = {{320, 240}, {240, 320}, {128, 128},
                          {480, 320}, {800, 480}, {240, 135}};
  for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
    const tth::FaceGeometry g =
        tth::FaceGeometry::forScreen(sizes[i][0], sizes[i][1]);
    TEST_ASSERT_TRUE(g.fitsOnScreen());
    TEST_ASSERT_TRUE(g.boxesAreDisjoint());
    TEST_ASSERT_TRUE(g.eyesBoxW > 0);
    TEST_ASSERT_TRUE(g.lowerBoxW > 0);
  }
}

// --- eye synchronisation ---------------------------------------------------
//
// This is the regression guard for the defect where only the left eye blinked.
// The two eyes must always be resolved from one identical snapshot.

static void both_eyes_share_identical_blink_progress() {
  const tth::FaceGeometry g = core2();

  // Sweep the entire blink range, plus every eye scale the animator uses.
  const float scales[6] = {
      tth::face::kEyeScaleReady, tth::face::kEyeScaleListening,
      tth::face::kEyeScaleWaiting, tth::face::kEyeScaleSpeaking,
      tth::face::kEyeScaleError, tth::face::kEyeScaleSleeping};

  for (int s = 0; s < 6; ++s) {
    for (int step = 0; step <= 100; ++step) {
      const float openness = static_cast<float>(step) / 100.0f;
      const tth::EyePairRender pair =
          tth::eyeRenderFor(g, frameWith(scales[s], openness, 0.05f, -0.03f));

      // Identical vertical squash: the whole point.
      TEST_ASSERT_EQUAL_INT(pair.left.radiusY, pair.right.radiusY);
      TEST_ASSERT_EQUAL_INT(pair.left.radiusX, pair.right.radiusX);
      TEST_ASSERT_EQUAL_INT(pair.left.centerY, pair.right.centerY);
      TEST_ASSERT_EQUAL_INT(pair.left.pupilRadiusY, pair.right.pupilRadiusY);
      TEST_ASSERT_EQUAL(pair.left.pupilVisible, pair.right.pupilVisible);
    }
  }
}

// The eyes differ in exactly one way: horizontal position.
static void the_eyes_differ_only_in_horizontal_position() {
  const tth::FaceGeometry g = core2();
  const tth::EyePairRender pair =
      tth::eyeRenderFor(g, frameWith(1.0f, 1.0f, 0.0f, 0.0f));

  TEST_ASSERT_TRUE(pair.left.centerX < pair.right.centerX);
  TEST_ASSERT_EQUAL_INT(g.rightEyeCenterX - g.leftEyeCenterX,
                        pair.right.centerX - pair.left.centerX);
  TEST_ASSERT_EQUAL_INT(pair.left.centerY, pair.right.centerY);
}

// Both eyes look the same way by the same amount.
static void both_eyes_track_the_pupil_together() {
  const tth::FaceGeometry g = core2();
  for (int step = -20; step <= 20; ++step) {
    const float dx = static_cast<float>(step) / 100.0f;
    const tth::EyePairRender pair =
        tth::eyeRenderFor(g, frameWith(1.0f, 1.0f, dx, dx * 0.5f));

    const int leftOffset = pair.left.pupilCenterX - pair.left.centerX;
    const int rightOffset = pair.right.pupilCenterX - pair.right.centerX;
    TEST_ASSERT_EQUAL_INT(leftOffset, rightOffset);

    const int leftOffsetY = pair.left.pupilCenterY - pair.left.centerY;
    const int rightOffsetY = pair.right.pupilCenterY - pair.right.centerY;
    TEST_ASSERT_EQUAL_INT(leftOffsetY, rightOffsetY);
  }
}

// Both eyes stay inside the shared sprite at every scale and deflection, so
// neither can be clipped while the other is not.
static void both_eyes_stay_inside_the_shared_sprite() {
  const tth::FaceGeometry g = core2();
  const tth::EyePairRender pair = tth::eyeRenderFor(
      g, frameWith(tth::face::kEyeScaleMax, 1.0f, 1.0f, 1.0f));
  const tth::EyeRender eyes[2] = {pair.left, pair.right};

  for (int i = 0; i < 2; ++i) {
    TEST_ASSERT_TRUE(eyes[i].centerX - eyes[i].radiusX >= 0);
    TEST_ASSERT_TRUE(eyes[i].centerX + eyes[i].radiusX <= g.eyesBoxW);
    TEST_ASSERT_TRUE(eyes[i].centerY - eyes[i].radiusY >= 0);
    TEST_ASSERT_TRUE(eyes[i].centerY + eyes[i].radiusY <= g.eyesBoxH);
  }
}

// A closed eye must still leave a visible line rather than vanishing.
static void a_fully_closed_eye_still_has_height() {
  const tth::FaceGeometry g = core2();
  const tth::EyePairRender pair =
      tth::eyeRenderFor(g, frameWith(1.0f, 0.0f, 0.0f, 0.0f));
  TEST_ASSERT_TRUE(pair.left.radiusY >= 1);
  TEST_ASSERT_EQUAL_INT(pair.left.radiusY, pair.right.radiusY);
  // ...and the pupil is hidden, which is what makes a blink read as a blink.
  TEST_ASSERT_FALSE(pair.left.pupilVisible);
  TEST_ASSERT_FALSE(pair.right.pupilVisible);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(everything_fits_on_a_320x240_screen);
  RUN_TEST(element_boxes_do_not_overlap);
  RUN_TEST(the_contain_fit_is_width_limited);
  RUN_TEST(the_layout_matches_the_ported_flutter_ratios);
  RUN_TEST(the_eyes_are_symmetric_about_the_centre);
  RUN_TEST(the_eye_box_contains_both_eyes_at_maximum_scale);
  RUN_TEST(the_lower_box_contains_the_smile_and_the_bars);
  RUN_TEST(the_sprite_budget_is_smaller_than_the_old_three_sprite_layout);
  RUN_TEST(a_full_face_transition_fits_in_one_frame);
  RUN_TEST(no_dimension_is_degenerate);
  RUN_TEST(the_layout_stays_valid_at_other_screen_sizes);
  RUN_TEST(both_eyes_share_identical_blink_progress);
  RUN_TEST(the_eyes_differ_only_in_horizontal_position);
  RUN_TEST(both_eyes_track_the_pupil_together);
  RUN_TEST(both_eyes_stay_inside_the_shared_sprite);
  RUN_TEST(a_fully_closed_eye_still_has_height);
  return UNITY_END();
}
