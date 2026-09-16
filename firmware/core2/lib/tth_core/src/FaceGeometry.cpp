#include "tth/FaceGeometry.h"

#include <math.h>

#include "tth/FaceAnimator.h"

namespace tth {

namespace {

int roundToInt(float v) { return static_cast<int>(floorf(v + 0.5f)); }
int ceilToInt(float v) { return static_cast<int>(ceilf(v)); }
int floorToInt(float v) { return static_cast<int>(floorf(v)); }

// Nudges a sprite box back inside the screen.
//
// Box sizes are rounded UP (so the element always has room) while box centres
// are rounded to nearest, and the contain-fit scale can leave a sub-pixel
// vertical offset. On a height-limited screen those can combine to put a box
// one pixel past the edge -- harmless-looking, but M5GFX would clip the push
// and the element would be cut off. Shifting the box by that pixel moves the
// drawn element by less than one pixel relative to its box centre, which is
// invisible, and guarantees every sprite is fully on screen.
void fitBoxWithin(int& origin, int size, int limit) {
  if (origin + size > limit) origin = limit - size;
  if (origin < 0) origin = 0;
}

float clamp01(float v) {
  if (v < 0.0f) return 0.0f;
  if (v > 1.0f) return 1.0f;
  return v;
}

}  // namespace

FaceGeometry FaceGeometry::forScreen(int screenWidth, int screenHeight) {
  FaceGeometry g;
  g.screenWidth = screenWidth;
  g.screenHeight = screenHeight;

  // --- design-space composition, exactly as the Dart version computes it ---
  const float eyeRadius = kFaceDesignWidth * kEyeRadiusRatio;
  const float eyeSpacing = kFaceDesignWidth * kEyeSpacingRatio;
  const float eyeRowHeight = eyeRadius * 2.0f * kEyeRowHeadroom;

  const float mouthWidth = kFaceDesignWidth * kMouthWidthRatio;
  const float mouthHeight = mouthWidth * kMouthHeightRatio;
  const float mouthRowHeight = mouthHeight * kMouthRowHeadroom;

  const float designHeight = eyeRowHeight + kEyeToMouthGap + mouthRowHeight;

  // --- uniform contain fit, the BoxFit.contain equivalent ---
  const float scaleX = static_cast<float>(screenWidth) / kFaceDesignWidth;
  const float scaleY = static_cast<float>(screenHeight) / designHeight;
  g.scale = scaleX < scaleY ? scaleX : scaleY;

  const float yOffset =
      (static_cast<float>(screenHeight) - designHeight * g.scale) * 0.5f;
  const float centreX = static_cast<float>(screenWidth) * 0.5f;

  // --- eye centres ---
  g.eyeRadius = eyeRadius * g.scale;
  const float eyeOffsetX = (eyeSpacing * 0.5f + eyeRadius) * g.scale;
  g.leftEyeCenterX = roundToInt(centreX - eyeOffsetX);
  g.rightEyeCenterX = roundToInt(centreX + eyeOffsetX);
  g.eyeCenterY = roundToInt(yOffset + (eyeRowHeight * 0.5f) * g.scale);

  // --- combined eye box, sized to the largest eye the animator can produce ---
  const float maxEyeRadius = g.eyeRadius * face::kEyeScaleMax;
  const int eyesLeft =
      floorToInt(static_cast<float>(g.leftEyeCenterX) - maxEyeRadius) -
      kBoxMargin;
  const int eyesRight =
      ceilToInt(static_cast<float>(g.rightEyeCenterX) + maxEyeRadius) +
      kBoxMargin;
  g.eyesBoxX = eyesLeft;
  g.eyesBoxW = eyesRight - eyesLeft;
  g.eyesBoxH = ceilToInt(2.0f * maxEyeRadius) + (2 * kBoxMargin);
  g.eyesBoxY = g.eyeCenterY - g.eyesBoxH / 2;

  // --- mouth ---
  g.mouthWidth = mouthWidth * g.scale;
  g.mouthHeight = mouthHeight * g.scale;
  g.mouthCenterX = roundToInt(centreX);
  g.mouthCenterY = roundToInt(
      yOffset +
      (eyeRowHeight + kEyeToMouthGap + mouthRowHeight * 0.5f) * g.scale);

  // --- audio bars, flanking the smile ---
  g.barHalfWidth = roundToInt(g.mouthWidth * kBarWidthRatio * 0.5f);
  if (g.barHalfWidth < 1) g.barHalfWidth = 1;
  const int barPitch = (2 * g.barHalfWidth) + 1 +
                       roundToInt(g.mouthWidth * kBarGapRatio);
  const int smileGap = roundToInt(g.mouthWidth * kBarSmileGapRatio);

  g.barCenterY =
      g.mouthCenterY + roundToInt(g.mouthHeight * kBarCenterYOffsetRatio);

  // The innermost bar sits one gap plus its own half-width clear of the smile;
  // the rest step outwards by a fixed pitch. Positions are fixed -- they never
  // move with the amplitude.
  const int innerLeftCenter =
      roundToInt(g.smileLeftEdge()) - smileGap - g.barHalfWidth;

  for (int i = 0; i < kBarsPerSide; ++i) {
    // i == 0 is the OUTERMOST bar, so it steps furthest from the smile.
    const int stepsOut = (kBarsPerSide - 1) - i;
    const int leftCenter = innerLeftCenter - (stepsOut * barPitch);
    const int rightCenter = (2 * g.mouthCenterX) - leftCenter;

    g.barCenterX[i] = leftCenter;
    g.barCenterX[kBarCount - 1 - i] = rightCenter;

    const int maxHalfHeight =
        roundToInt(g.mouthHeight * kBarMaxHalfHeightRatio[i]);
    g.barMaxHalfHeight[i] = maxHalfHeight;
    g.barMaxHalfHeight[kBarCount - 1 - i] = maxHalfHeight;
  }

  // --- lower-face box: must contain the smile AND every bar at full height ---
  const float curve = g.mouthHeight * face::kMouthCurveRatio;
  const float smileHalf = 0.5f * g.mouthHeight * face::kSmileThicknessRatio;

  // The smile's topmost point is at its ends (where the bow is zero) and its
  // lowest is at the centre.
  float top = static_cast<float>(g.mouthCenterY) - smileHalf;
  float bottom = static_cast<float>(g.mouthCenterY) + curve + smileHalf;

  int leftMost = roundToInt(g.smileLeftEdge());
  int rightMost = roundToInt(g.smileRightEdge());

  for (int i = 0; i < kBarCount; ++i) {
    const float barTop =
        static_cast<float>(g.barCenterY - g.barMaxHalfHeight[i]);
    const float barBottom =
        static_cast<float>(g.barCenterY + g.barMaxHalfHeight[i]);
    if (barTop < top) top = barTop;
    if (barBottom > bottom) bottom = barBottom;

    const int barLeft = g.barCenterX[i] - g.barHalfWidth;
    const int barRight = g.barCenterX[i] + g.barHalfWidth;
    if (barLeft < leftMost) leftMost = barLeft;
    if (barRight > rightMost) rightMost = barRight;
  }

  g.lowerBoxX = leftMost - kBoxMargin;
  g.lowerBoxW = (rightMost + kBoxMargin) - g.lowerBoxX;
  g.lowerBoxY = floorToInt(top) - kBoxMargin;
  g.lowerBoxH = (ceilToInt(bottom) + kBoxMargin) - g.lowerBoxY;

  fitBoxWithin(g.eyesBoxX, g.eyesBoxW, g.screenWidth);
  fitBoxWithin(g.eyesBoxY, g.eyesBoxH, g.screenHeight);
  fitBoxWithin(g.lowerBoxX, g.lowerBoxW, g.screenWidth);
  fitBoxWithin(g.lowerBoxY, g.lowerBoxH, g.screenHeight);

  return g;
}

bool FaceGeometry::fitsOnScreen() const {
  if (eyesBoxX < 0 || eyesBoxY < 0 || lowerBoxX < 0 || lowerBoxY < 0) {
    return false;
  }
  if (eyesBoxX + eyesBoxW > screenWidth) return false;
  if (eyesBoxY + eyesBoxH > screenHeight) return false;
  if (lowerBoxX + lowerBoxW > screenWidth) return false;
  if (lowerBoxY + lowerBoxH > screenHeight) return false;
  return true;
}

bool FaceGeometry::boxesAreDisjoint() const {
  return (eyesBoxY + eyesBoxH) <= lowerBoxY;
}

int FaceGeometry::spriteBytes() const {
  const int bytesPerPixel = 2;  // 16bpp
  return ((eyesBoxW * eyesBoxH) + (lowerBoxW * lowerBoxH)) * bytesPerPixel;
}

int FaceGeometry::fullFacePixels() const {
  return (eyesBoxW * eyesBoxH) + (lowerBoxW * lowerBoxH);
}

namespace {

EyeRender resolveEye(const FaceGeometry& g, const FaceFrame& frame,
                     int screenCenterX) {
  EyeRender eye;
  const float radius = g.eyeRadius * frame.eyeScale;

  eye.centerX = screenCenterX - g.eyesBoxX;
  eye.centerY = g.eyeCenterY - g.eyesBoxY;

  // A blink squashes the eye vertically rather than shrinking it, so the eye
  // keeps its width the whole way down -- which is what makes it read as an
  // eyelid closing.
  eye.radiusX = static_cast<int>(radius + 0.5f);
  eye.radiusY = static_cast<int>((radius * frame.eyeOpenness) + 0.5f);
  if (eye.radiusY < 1) eye.radiusY = 1;  // closed still shows a thin line

  eye.pupilVisible = frame.eyeOpenness > face::kPupilVisibleOpenness;

  const float pupilR = radius * face::kPupilRadiusRatio;
  eye.pupilCenterX =
      eye.centerX +
      static_cast<int>((frame.pupilDx * radius * face::kPupilTravel) + 0.5f);
  eye.pupilCenterY =
      eye.centerY +
      static_cast<int>((frame.pupilDy * radius * face::kPupilTravel) + 0.5f);
  eye.pupilRadiusX = static_cast<int>(pupilR + 0.5f);
  eye.pupilRadiusY = static_cast<int>((pupilR * frame.eyeOpenness) + 0.5f);
  if (eye.pupilRadiusY < 1) eye.pupilRadiusY = 1;

  return eye;
}

}  // namespace

EyePairRender eyeRenderFor(const FaceGeometry& geometry,
                           const FaceFrame& frame) {
  EyePairRender pair;
  // Both eyes resolved from the SAME frame in the SAME call. There is no path
  // by which one could be computed from a newer frame than the other.
  pair.left = resolveEye(geometry, frame, geometry.leftEyeCenterX);
  pair.right = resolveEye(geometry, frame, geometry.rightEyeCenterX);
  return pair;
}

void audioBarsFor(const FaceGeometry& geometry, const FaceFrame& frame,
                  BarRender out[kBarCount]) {
  const float level = clamp01(frame.speechLevel);

  // Below the fade point the bar dims as well as shrinking, so it goes away
  // gradually rather than blinking out.
  const float intensity =
      level >= face::kBarFadeBelow ? 1.0f : (level / face::kBarFadeBelow);

  for (int i = 0; i < kBarCount; ++i) {
    BarRender& bar = out[i];
    bar.centerX = geometry.barCenterX[i] - geometry.lowerBoxX;
    bar.centerY = geometry.barCenterY - geometry.lowerBoxY;
    bar.halfWidth = geometry.barHalfWidth;
    bar.halfHeight = static_cast<int>(
        (static_cast<float>(geometry.barMaxHalfHeight[i]) * level) + 0.5f);
    bar.intensity = intensity;
    // A bar shorter than it is wide is just a dot; at that point it has
    // effectively disappeared, which is what silence must look like.
    bar.visible = level > 0.0f && bar.halfHeight >= geometry.barHalfWidth;
  }
}

}  // namespace tth
