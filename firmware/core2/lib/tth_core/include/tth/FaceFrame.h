#pragma once

// One rendered frame's worth of face parameters, all normalised. The renderer
// alone deals in pixels.
//
// There is exactly ONE eyeOpenness and ONE eyeScale for the whole face, not a
// pair. That is deliberate and structural: the two eyes are physically
// incapable of showing different blink phases because there is only one value
// to draw them from.
//
// There is no mouth-opening parameter. The mouth is a fixed friendly smile in
// every state and never opens, stretches or deforms -- an animated open mouth
// read as a frightened grimace on the real device. Speech is shown instead by
// the audio level bars flanking the smile, driven by `speechLevel`.

namespace tth {

struct FaceFrame {
  float eyeScale;     // multiplier on FaceGeometry::eyeRadius
  float eyeOpenness;  // 0 = closed (blinking), 1 = fully open
  float pupilDx;      // -1..1, fraction of the pupil's travel range
  float pupilDy;      // -1..1
  float speechLevel;  // 0 = silent, no bars drawn; 1 = loudest
  bool dim;           // draw in the dim cyan rather than the primary cyan
};

}  // namespace tth
