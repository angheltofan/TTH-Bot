import 'dart:async';
import 'dart:math';

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';

import '../../core/theme/tth_colors.dart';
import 'robot_expression.dart';
import 'robot_eyes.dart';
import 'robot_face_geometry.dart';
import 'robot_mouth.dart';

/// The robot's face: two eyes and a mouth on a dark background — no
/// eyebrows (removed by design revision — see [RobotFaceGeometry]'s doc),
/// no head outline, ears, antenna or body. Expression is carried entirely
/// by eye size/pupils and mouth shape.
///
/// Laid out once on a fixed-width reference canvas ([_designWidth]) sized
/// to content ([Column] with `mainAxisSize.min`, so it never has a height
/// ceiling to overflow), then the whole canvas is scaled uniformly to fit
/// the real device via [FittedBox]. That's what makes this responsive
/// without hardcoded device-specific pixel math: a previous version derived
/// sizes from [LayoutBuilder]'s *width* only and fed them straight into a
/// height-constrained [Column], which overflowed on real (wider-than-tall,
/// height-limited) phone screens — [FittedBox] makes that class of bug
/// impossible, since its child always lays out against its own natural
/// size rather than the parent's constraints.
///
/// [RobotFaceGeometry] centralizes the actual proportions (eye radius,
/// spacing, eyebrow/mouth width, ...) computed from [_designWidth]; see its
/// doc for why it's still the eye/eyebrow/mouth widgets' `width`/`geometry`
/// parameters — not absolute coordinates — that drive the [Column] layout.
///
/// Native Flutter [CustomPainter]s, not Rive — no `.riv` asset exists in
/// this project yet (see Phase 5 baseline). Rendering is isolated behind
/// this widget and [RobotExpression] so a future Rive swap wouldn't need to
/// touch [VoiceSessionController], Supabase or activity logic.
class RobotFace extends StatefulWidget {
  const RobotFace({
    super.key,
    required this.expression,
    required this.amplitude,
  });

  final RobotExpression expression;
  final ValueListenable<double> amplitude;

  @override
  State<RobotFace> createState() => _RobotFaceState();
}

class _RobotFaceState extends State<RobotFace>
    with SingleTickerProviderStateMixin {
  late final AnimationController _blinkController;
  final Random _random = Random();
  Timer? _blinkTimer;

  bool get _canBlink =>
      widget.expression == RobotExpression.neutral ||
      widget.expression == RobotExpression.listening ||
      widget.expression == RobotExpression.encouraging;

  @override
  void initState() {
    super.initState();
    _blinkController = AnimationController(
      vsync: this,
      duration: const Duration(milliseconds: 150),
    );
    _scheduleNextBlink();
  }

  void _scheduleNextBlink() {
    // Randomized interval (not a fixed robotic timer) so idle blinking
    // reads as alive rather than mechanical.
    final delay = Duration(milliseconds: 2500 + _random.nextInt(3500));
    _blinkTimer = Timer(delay, () {
      if (!mounted) return;
      if (!_canBlink) {
        _scheduleNextBlink();
        return;
      }
      _blink(
        onDone: () {
          if (!mounted) return;
          // Occasional natural double-blink — a quick second blink right
          // after the first, the way people sometimes actually blink.
          if (_random.nextDouble() < 0.22) {
            Timer(const Duration(milliseconds: 160), () {
              if (!mounted) return;
              _blink(onDone: _scheduleNextBlink);
            });
          } else {
            _scheduleNextBlink();
          }
        },
      );
    });
  }

  void _blink({required VoidCallback onDone}) {
    _blinkController.forward().then((_) {
      if (!mounted) return;
      _blinkController.reverse().then((_) {
        if (mounted) onDone();
      });
    });
  }

  @override
  void dispose() {
    _blinkTimer?.cancel();
    _blinkController.dispose();
    super.dispose();
  }

  // Arbitrary reference unit, not a device pixel size — FittedBox scales
  // this whole canvas to fit the real screen, so only the *ratios* baked
  // into RobotFaceGeometry matter.
  static const double _designWidth = 340;

  @override
  Widget build(BuildContext context) {
    final geometry = RobotFaceGeometry.fromWidth(_designWidth);
    return ColoredBox(
      color: TthColors.faceBackground,
      child: Center(
        child: FittedBox(
          fit: BoxFit.contain,
          child: SizedBox(
            width: _designWidth,
            child: AnimatedBuilder(
              animation: _blinkController,
              builder: (context, _) {
                final openness = 1.0 - _blinkController.value;
                return Column(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    RobotEyes(
                      expression: widget.expression,
                      openness: openness,
                      geometry: geometry,
                    ),
                    const SizedBox(height: 14),
                    RobotMouth(
                      expression: widget.expression,
                      amplitude: widget.amplitude,
                      width: geometry.mouthWidth,
                    ),
                  ],
                );
              },
            ),
          ),
        ),
      ),
    );
  }
}
