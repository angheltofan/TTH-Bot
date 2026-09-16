#pragma once

#include <stdint.h>

#include "tth/FaceFrame.h"

// Robot face geometry, ported from the Flutter implementation.
//
// The Flutter face (lib/features/robot/robot_face_geometry.dart) lays itself
// out on a fixed-width reference canvas and then scales the whole composition
// uniformly with FittedBox(BoxFit.contain). That is reproduced exactly here:
// the same reference width, the same ratios, the same contain-fit -- only the
// target is 320x240 instead of a phone screen.
//
// TWO SPRITES
//
// Both eyes share ONE sprite: they must never show different blink phases, and
// the only way to guarantee that rather than merely arrange it is to make them
// a single sprite drawn from a single FaceFrame.
//
// The fixed smile and both groups of audio bars share ONE lower-face sprite,
// so the smile and the bars are always updated together and can never be seen
// half-changed.
//
// Boxes are sized to the largest extent the animator can actually produce,
// which is what keeps the pushed pixel count -- and therefore the transition
// time on a 40 MHz SPI bus -- inside one display frame.

namespace tth {

// --- Ratios, copied from RobotFaceGeometry.fromWidth() ---------------------

// Arbitrary reference unit, not a pixel size. Only the ratios matter.
const float kFaceDesignWidth = 340.0f;

const float kEyeRadiusRatio = 0.155f;   // of design width
const float kEyeSpacingRatio = 0.14f;   // gap between the eyes' near edges
const float kMouthWidthRatio = 0.46f;   // of design width
const float kMouthHeightRatio = 0.5f;   // of mouth width
const float kEyeToMouthGap = 14.0f;     // in design units

// Row heights still use the Dart headroom constants, because they set where
// the rows SIT relative to one another. The sprite boxes are sized separately.
const float kEyeRowHeadroom = 1.3f;
const float kMouthRowHeadroom = 1.3f;

// A pixel or two of slack around each box, so rounding can never clip an edge.
const int kBoxMargin = 2;

// --- Audio level bars -------------------------------------------------------
//
// Three vertical rounded bars either side of the fixed smile, symmetrically
// placed at fixed horizontal positions. They are the ONLY thing that indicates
// speech; the face itself does not change.

const int kBarsPerSide = 3;
const int kBarCount = kBarsPerSide * 2;

const float kBarWidthRatio = 0.040f;      // of mouth width
const float kBarGapRatio = 0.034f;        // of mouth width, between bars
const float kBarSmileGapRatio = 0.048f;   // of mouth width, smile edge -> bar

// Maximum half-heights, as fractions of the mouth height, from the outermost
// bar inwards. Inner bars are slightly taller. Deliberately modest: the bars
// must stay well clear of the eyes.
const float kBarMaxHalfHeightRatio[kBarsPerSide] = {0.17f, 0.23f, 0.29f};

// Bars sit slightly below the mouth centre line, so they read as flanking the
// smile rather than floating above it.
const float kBarCenterYOffsetRatio = 0.08f;  // of mouth height

struct FaceGeometry {
  int screenWidth;
  int screenHeight;

  // Uniform contain-fit scale from design units to pixels.
  float scale;

  // --- eyes ---
  float eyeRadius;  // base (unscaled by expression) radius, in pixels
  int leftEyeCenterX;
  int rightEyeCenterX;
  int eyeCenterY;

  // The single sprite holding BOTH eyes.
  int eyesBoxX;
  int eyesBoxY;
  int eyesBoxW;
  int eyesBoxH;

  // --- lower face: the fixed smile plus both bar groups, one sprite ---
  float mouthWidth;   // base width in pixels
  float mouthHeight;  // base height in pixels
  int mouthCenterX;
  int mouthCenterY;

  int barCenterY;
  int barHalfWidth;
  // Index 0..2 are the left bars, outermost first; 3..5 the right bars,
  // innermost first. So bars i and (kBarCount - 1 - i) are mirror pairs.
  int barCenterX[kBarCount];
  int barMaxHalfHeight[kBarCount];

  int lowerBoxX;
  int lowerBoxY;
  int lowerBoxW;
  int lowerBoxH;

  static FaceGeometry forScreen(int screenWidth, int screenHeight);

  bool fitsOnScreen() const;

  // The eye box and the lower-face box do not overlap, so redrawing one never
  // disturbs the other.
  bool boxesAreDisjoint() const;

  // Total bytes for the two sprites at 16 bits per pixel.
  int spriteBytes() const;

  // Pixels pushed when the whole face is redrawn. At 40 MHz and 16 bits per
  // pixel this is 0.4 us each, which is what bounds a state transition.
  int fullFacePixels() const;

  // Horizontal extent of the fixed smile, in screen coordinates. Used to prove
  // the bars never touch it.
  float smileLeftEdge() const { return mouthCenterX - (mouthWidth * 0.5f); }
  float smileRightEdge() const { return mouthCenterX + (mouthWidth * 0.5f); }
};

// One eye's resolved drawing parameters, in sprite-local coordinates.
struct EyeRender {
  int centerX;
  int centerY;
  int radiusX;
  int radiusY;  // squashed by the blink
  int pupilCenterX;
  int pupilCenterY;
  int pupilRadiusX;
  int pupilRadiusY;
  bool pupilVisible;
};

struct EyePairRender {
  EyeRender left;
  EyeRender right;
};

// Resolves BOTH eyes from ONE frame, in one call. The renderer uses this
// rather than computing each eye separately, so left and right cannot diverge.
EyePairRender eyeRenderFor(const FaceGeometry& geometry, const FaceFrame& frame);

// One audio bar's resolved drawing parameters, in lower-face sprite
// coordinates.
struct BarRender {
  int centerX;
  int centerY;
  int halfWidth;
  int halfHeight;
  bool visible;     // false once the bar has shrunk away entirely
  float intensity;  // 0..1, fades the colour as the bar disappears
};

// Resolves all six bars from ONE frame. Bars shorter than they are wide are
// reported invisible, so silence leaves nothing but the smile.
void audioBarsFor(const FaceGeometry& geometry, const FaceFrame& frame,
                  BarRender out[kBarCount]);

}  // namespace tth
