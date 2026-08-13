import 'package:flutter/foundation.dart';

/// [RobotEyes]' `_Eye` reserves a fixed render box of
/// `eyeRadius * 2 * kEyeOuterHeadroom` per eye, regardless of the animated
/// expression scale (so a scaled-up eye — e.g. listening — never paints
/// outside its allotted space). Shared as a single constant, rather than
/// duplicated as a literal in both `robot_eyes.dart` and here, after a
/// previous revision duplicated it inconsistently and the two eyes'
/// combined width silently overflowed the design canvas (caught by
/// `robot_face_test.dart`, not by the geometry unit test, which is why
/// that test now also exists).
const double kEyeOuterHeadroom = 1.3;

/// Same idea as [kEyeOuterHeadroom], for [RobotMouth]'s fixed render-box
/// headroom (covers the largest expression scale plus a fully-open
/// speaking amplitude).
const double kMouthOuterHeadroom = 1.3;

/// Named, ratio-derived face geometry, computed once from the face's
/// reference width (see [RobotFace]'s doc for why a fixed reference width
/// scaled uniformly via `FittedBox` — not per-device pixel math — is what
/// makes the face responsive without ever overflowing).
///
/// Two rows only — eyes, then mouth. No eyebrow row: per this design
/// revision the face carries its expression entirely through eye size/
/// pupils and mouth shape, so eyebrows were removed rather than just made
/// smaller (see `robot_eyebrows.dart`'s deletion).
///
/// Centralizes the proportions that used to be scattered as inline
/// `width * 0.NN` literals through [RobotEyes]/[RobotMouth] into one
/// reviewable, directly unit-testable place — see
/// `robot_face_geometry_test.dart` for validity checks (no negative/NaN/
/// overflowing values) across a range of landscape sizes.
@immutable
class RobotFaceGeometry {
  const RobotFaceGeometry({
    required this.faceWidth,
    required this.faceHeight,
    required this.eyeRadius,
    required this.eyeSpacing,
    required this.eyeCenterY,
    required this.mouthWidth,
    required this.mouthCenterY,
  });

  /// The reference width the whole face composition is derived from.
  final double faceWidth;

  /// Total height of the eyes+mouth composition at this width —
  /// informational (validity-testable), not consumed as a hard layout
  /// constraint: the actual widget tree sizes itself to content
  /// ([Column] with `mainAxisSize.min`) and relies on `FittedBox` to fit
  /// whatever that turns out to be, rather than the other way around.
  final double faceHeight;

  /// Base (unscaled) eye radius — large, since the eyes are now the face's
  /// main visual focus with eyebrows gone.
  final double eyeRadius;

  /// Gap between the two eyes' nearest edges — see [RobotEyes].
  final double eyeSpacing;

  /// Vertical center of the eye row, measured from the top of the face
  /// composition.
  final double eyeCenterY;

  /// Base (unscaled) mouth width — the reference span [RobotMouth] scales
  /// per expression, same pattern as [eyeRadius] for the eyes.
  final double mouthWidth;

  /// Vertical center of the mouth, measured from the top of the face
  /// composition.
  final double mouthCenterY;

  // Kept in sync with the SizedBox gap RobotFace actually lays out between
  // the two rows — tight, so FittedBox(BoxFit.contain) scales the whole
  // composition up as much as the (height-limited, landscape-phone)
  // screen allows.
  static const double _eyeToMouthGap = 14;

  factory RobotFaceGeometry.fromWidth(double width) {
    assert(width > 0, 'RobotFaceGeometry requires a positive width');

    // Large — the eyes are the main visual focus now that there are no
    // eyebrows competing for attention — with a clear gap between them,
    // matching the reference image (previously spaced quite tight).
    // Bounded by _Eye's fixed outer render headroom (size *
    // kEyeOuterHeadroom, applied per eye regardless of expression scale):
    // the two eyes' outer boxes plus their gap must fit within `width`,
    // i.e. 4 * kEyeOuterHeadroom * eyeRadius + eyeSpacing <= width —
    // verified by robot_face_geometry_test.dart and a RobotFace-level
    // render smoke test (robot_face_test.dart) after an earlier ratio
    // overflowed.
    final eyeRadius = width * 0.155;
    final eyeSpacing = width * 0.14;
    final eyeRowHeight = eyeRadius * 2 * kEyeOuterHeadroom;

    // A small, clean, filled cyan *ring* now (see robot_mouth.dart) — a
    // deliberately modest span, dialed back again after an earlier
    // over-large attempt ("smaller mouth than before").
    final mouthWidth = width * 0.46;
    final mouthBaseHeight = mouthWidth * 0.5;
    final mouthRowHeight = mouthBaseHeight * kMouthOuterHeadroom;

    final eyeCenterY = eyeRowHeight / 2;
    final mouthCenterY = eyeRowHeight + _eyeToMouthGap + mouthRowHeight / 2;
    final faceHeight = mouthCenterY + mouthRowHeight / 2;

    return RobotFaceGeometry(
      faceWidth: width,
      faceHeight: faceHeight,
      eyeRadius: eyeRadius,
      eyeSpacing: eyeSpacing,
      eyeCenterY: eyeCenterY,
      mouthWidth: mouthWidth,
      mouthCenterY: mouthCenterY,
    );
  }
}
