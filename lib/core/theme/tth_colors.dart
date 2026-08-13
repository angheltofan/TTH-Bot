import 'package:flutter/material.dart';

/// Tales & Tech Hub brand palette. Two independent uses:
///
/// * [web] — light, centered configuration UI (`flutter run -d chrome`).
/// * [face] — dark robot-face palette for the physical Android device;
///   deliberately unrelated to the web palette (see Phase 5 spec: "use a
///   different functional palette from the light Web interface").
abstract final class TthColors {
  /// Primary Tales & Tech Hub blue.
  static const Color brandBlue = Color(0xFF017ED6);

  /// Tales & Tech Hub accent orange, used sparingly for primary CTAs.
  static const Color brandOrange = Color(0xFFFB8E21);

  static const Color white = Color(0xFFFFFFFF);

  /// Very light blue/neutral page background for the web app.
  static const Color webBackground = Color(0xFFF7FBFE);

  /// Deep navy text color that complements [brandBlue] without competing
  /// with it.
  static const Color webTextDark = Color(0xFF0B2438);

  static const Color webTextMuted = Color(0xFF4C6478);

  static const Color webCardBorder = Color(0xFFE1ECF5);

  /// Deep navy/near-black background for the robot face.
  static const Color faceBackground = Color(0xFF060F1A);

  /// Soft electric-cyan used for eyes/eyebrows/mouth.
  static const Color faceCyan = Color(0xFF4DEFFF);

  /// Dimmer cyan for offline/low-emphasis face states.
  static const Color faceCyanDim = Color(0xFF2C6E78);
}
