import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';

import '../../core/theme/tth_colors.dart';
import 'robot_expression.dart';
import 'robot_face_geometry.dart' show kMouthOuterHeadroom;

/// Mouth reacting to [expression]. A small, clean, filled cyan **ring**
/// tracing a rounded open-smile silhouette — computed as the boolean
/// difference between an outer and an inner copy of the same smile-lens
/// path (see [_MouthPainter.paint]), not a stroked line. That choice is
/// deliberate: an earlier version stroked a thin, sharply-pointed bezier
/// path with a constant-width blurred stroke, which self-overlapped at the
/// pointed corners and bloomed outward from the blur — it read as a
/// blown-out, "ghosted" second mouth rather than one clean shape. A filled
/// ring has no such failure mode: its border is exact 2D geometry at every
/// point, including the corners, where it naturally (and correctly) tapers
/// to nothing.
///
/// One shared shape family ([_mouthLensPath]/[_MouthStyle]) drives every
/// state, including [RobotExpression.speaking], which still redraws
/// continuously from [amplitude] (see
/// `VoiceSessionController.speakingAmplitude`) so the mouth opens/closes
/// with the actual audio rather than a canned animation — approximate, not
/// phoneme-accurate lip sync. Reusing the same shape family for speaking
/// (amplitude only opens it further) is what keeps the "same cheerful
/// character" whether idle, listening or talking, in both push-to-talk and
/// free-conversation — this widget takes no `InteractionMode` at all.
class RobotMouth extends StatelessWidget {
  const RobotMouth({
    super.key,
    required this.expression,
    required this.amplitude,
    required this.width,
  });

  final RobotExpression expression;
  final ValueListenable<double> amplitude;

  /// Base (unscaled) mouth width — see [RobotFaceGeometry.mouthWidth].
  final double width;

  static const double _baseHeight = 0.5; // of `width`

  @override
  Widget build(BuildContext context) {
    final outerWidth = width * kMouthOuterHeadroom;
    final outerHeight = width * _baseHeight * kMouthOuterHeadroom;

    if (expression == RobotExpression.speaking) {
      // Keyed separately so amplitude ticks never re-trigger the
      // cross-fade transition below — only an actual expression change does.
      return SizedBox(
        key: const ValueKey('speaking'),
        width: outerWidth,
        height: outerHeight,
        child: Center(
          child: ValueListenableBuilder<double>(
            valueListenable: amplitude,
            builder: (context, value, _) {
              final style = _speakingStyleFor(value.clamp(0.0, 1.0));
              return SizedBox(
                width: width * style.scale,
                height: width * _baseHeight * style.scale,
                child: CustomPaint(painter: _MouthPainter(style, dim: false)),
              );
            },
          ),
        ),
      );
    }

    return SizedBox(
      width: outerWidth,
      height: outerHeight,
      child: Center(
        child: TweenAnimationBuilder<_MouthStyle>(
          tween: _MouthStyleTween(
            begin: _styleFor(expression),
            end: _styleFor(expression),
          ),
          duration: const Duration(milliseconds: 260),
          curve: Curves.easeInOutCubic,
          builder: (context, style, _) {
            return SizedBox(
              width: width * style.scale,
              height: width * _baseHeight * style.scale,
              child: CustomPaint(
                painter: _MouthPainter(
                  style,
                  dim: expression == RobotExpression.offline,
                ),
              ),
            );
          },
        ),
      ),
    );
  }
}

/// Continuously-interpolatable mouth shape parameters, all normalized to
/// the mouth's own bounding box:
///
/// * [scale] — overall size, relative to the base width/height.
/// * [curveDepth] — how much the smile bows, 0..~0.3 of the box height.
/// * [thickness] — how "open" the mouth looks, 0..~0.6 of the box height —
///   the outer silhouette's vertical span. Small = a thin cheerful smile;
///   large = a fuller open mouth. The visible cyan border itself is a
///   separate, fixed width (see `_ringBand` in [_MouthPainter]), not part
///   of this per-expression style — the border shouldn't get proportionally
///   thicker just because the mouth opens wider.
@immutable
class _MouthStyle {
  const _MouthStyle({
    required this.scale,
    required this.curveDepth,
    required this.thickness,
  });

  final double scale;
  final double curveDepth;
  final double thickness;

  static _MouthStyle lerp(_MouthStyle a, _MouthStyle b, double t) {
    return _MouthStyle(
      scale: _lerpD(a.scale, b.scale, t),
      curveDepth: _lerpD(a.curveDepth, b.curveDepth, t),
      thickness: _lerpD(a.thickness, b.thickness, t),
    );
  }
}

double _lerpD(double a, double b, double t) => a + (b - a) * t;

class _MouthStyleTween extends Tween<_MouthStyle> {
  _MouthStyleTween({required _MouthStyle begin, required _MouthStyle end})
    : super(begin: begin, end: end);

  @override
  _MouthStyle lerp(double t) => _MouthStyle.lerp(begin!, end!, t);
}

// Smaller across the board than the previous revision, per explicit
// feedback ("smaller mouth than before").
_MouthStyle _styleFor(RobotExpression expression) {
  switch (expression) {
    case RobotExpression.neutral:
      // Cheerful default — already a small open smiling shape at rest,
      // not a closed/flat line.
      return const _MouthStyle(scale: 1, curveDepth: 0.1, thickness: 0.28);
    case RobotExpression.listening:
      // Simply bigger (per the design brief: enlarge, don't invent a
      // separate grimace) and a touch more open — attentive, still
      // cheerful.
      return const _MouthStyle(scale: 1.12, curveDepth: 0.1, thickness: 0.34);
    case RobotExpression.thinking:
      return const _MouthStyle(scale: 0.8, curveDepth: 0.05, thickness: 0.12);
    case RobotExpression.asking:
      return const _MouthStyle(scale: 1, curveDepth: 0.07, thickness: 0.38);
    case RobotExpression.speaking:
      // Unreachable (handled by _speakingStyleFor) — kept only so the
      // switch stays exhaustive.
      return const _MouthStyle(scale: 1, curveDepth: 0.1, thickness: 0.28);
    case RobotExpression.happy:
      return const _MouthStyle(scale: 1.08, curveDepth: 0.18, thickness: 0.34);
    case RobotExpression.encouraging:
      // Warm — never less cheerful-looking than the default, just calmer.
      return const _MouthStyle(scale: 1, curveDepth: 0.1, thickness: 0.26);
    case RobotExpression.offline:
      // Calm/simplified, not broken-looking.
      return const _MouthStyle(scale: 0.65, curveDepth: 0.02, thickness: 0.05);
  }
}

/// Same cheerful shape family as [_styleFor]'s neutral case, with the real
/// PCM [amplitude] opening it further on top — a happy robot talking, not
/// a harsh mechanical open/close. Starts from the same open, cheerful
/// baseline as neutral (not closed), since that's already the resting
/// "happy talking" mouth.
_MouthStyle _speakingStyleFor(double amplitude) {
  return _MouthStyle(
    scale: 1 + amplitude * 0.06,
    curveDepth: 0.1 + amplitude * 0.03,
    thickness: 0.28 + amplitude * 0.34,
  );
}

class _MouthPainter extends CustomPainter {
  const _MouthPainter(this.style, {required this.dim});

  final _MouthStyle style;
  final bool dim;

  /// Fixed visible border width (normalized to box height) — constant
  /// across expressions/amplitude, so the ring never balloons into a
  /// blown-out mass on wide-open states; only the opening itself grows.
  static const double _ringBand = 0.1;

  @override
  void paint(Canvas canvas, Size size) {
    final color = dim ? TthColors.faceCyanDim : TthColors.faceCyan;
    final outer = _mouthLensPath(size, style);
    final innerStyle = _MouthStyle(
      scale: style.scale,
      curveDepth: style.curveDepth,
      thickness: (style.thickness - _ringBand).clamp(0.0, style.thickness),
    );
    final inner = _mouthLensPath(size, innerStyle);

    // Soft glow: one blurred fill of the *outer* silhouette, well beneath
    // everything else — a single soft halo, not a duplicated contour, so
    // it can never read as a second mouth.
    canvas.drawPath(
      outer,
      Paint()
        ..color = color.withValues(alpha: 0.22)
        ..maskFilter = const MaskFilter.blur(BlurStyle.normal, 6),
    );

    // Subtle dark cavity, so the opening reads as a real opening rather
    // than empty space.
    canvas.drawPath(
      inner,
      Paint()..color = TthColors.faceBackground.withValues(alpha: 0.7),
    );

    // The one crisp visible contour — computed as exact 2D geometry
    // (outer minus inner), so it stays clean at any thickness, including
    // where the two curves taper together at the corners.
    final ring = Path.combine(PathOperation.difference, outer, inner);
    canvas.drawPath(ring, Paint()..color = color);
  }

  @override
  bool shouldRepaint(covariant _MouthPainter oldDelegate) =>
      oldDelegate.style != style || oldDelegate.dim != dim;
}

/// A smile-curved lens: a top edge and a bottom edge sharing the same
/// left/right corner points, both bowed from the same vertical center
/// ([_MouthStyle.curveDepth]) by half of [_MouthStyle.thickness] each.
/// Small thickness pinches the two edges together into a thin cheerful
/// smile line; larger thickness opens them into a fuller mouth — one
/// continuous shape family, reused for both the outer and inner boundary
/// of [_MouthPainter]'s ring.
Path _mouthLensPath(Size size, _MouthStyle style) {
  final w = size.width * 0.9;
  final left = Offset(size.width / 2 - w / 2, size.height / 2);
  final right = Offset(size.width / 2 + w / 2, size.height / 2);
  final midX = size.width / 2;
  final baseY = size.height / 2 + size.height * style.curveDepth;
  final halfThickness = size.height * style.thickness / 2;
  final topControl = Offset(midX, baseY - halfThickness);
  final bottomControl = Offset(midX, baseY + halfThickness);

  return Path()
    ..moveTo(left.dx, left.dy)
    ..quadraticBezierTo(topControl.dx, topControl.dy, right.dx, right.dy)
    ..quadraticBezierTo(bottomControl.dx, bottomControl.dy, left.dx, left.dy)
    ..close();
}
