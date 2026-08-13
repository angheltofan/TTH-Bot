import 'dart:async';
import 'dart:math';

import 'package:flutter/material.dart';

import '../../core/theme/tth_colors.dart';
import 'robot_expression.dart';
import 'robot_face_geometry.dart';

/// Two eyes with pupils. [openness] (0=closed, 1=open) is driven
/// continuously by the parent [RobotFace]'s blink loop; per-[RobotExpression]
/// styling (scale, pupil position, whether the eye narrows into a happy
/// arc) is tweened smoothly on top of that whenever the expression changes.
///
/// A slow, small idle pupil drift (see [_idleController]) runs continuously
/// underneath the expression-driven gaze — "occasional very small eye
/// movements", not eyes that constantly dart around, per the design brief.
class RobotEyes extends StatefulWidget {
  const RobotEyes({
    super.key,
    required this.expression,
    required this.openness,
    required this.geometry,
  });

  final RobotExpression expression;
  final double openness;
  final RobotFaceGeometry geometry;

  @override
  State<RobotEyes> createState() => _RobotEyesState();
}

class _RobotEyesState extends State<RobotEyes>
    with SingleTickerProviderStateMixin {
  // Timer-scheduled discrete retargeting (like RobotFace's blink loop),
  // deliberately *not* a perpetually-repeating AnimationController: a
  // never-settling repeat() ticker makes `pumpAndSettle()` spin forever in
  // every widget test that renders a face, since it always has a pending
  // animation frame scheduled. Timer-driven one-shot animations don't have
  // that problem (same reason the blink loop is also Timer-driven) and
  // this reads as "occasional very small eye movements" even more
  // literally than a continuous wander would.
  late final AnimationController _idleController;
  final Random _random = Random();
  Timer? _idleTimer;
  Offset _idleFrom = Offset.zero;
  Offset _idleTo = Offset.zero;

  @override
  void initState() {
    super.initState();
    _idleController = AnimationController(
      vsync: this,
      duration: const Duration(milliseconds: 1800),
    );
    _scheduleNextIdleMove();
  }

  void _scheduleNextIdleMove() {
    final delay = Duration(milliseconds: 4000 + _random.nextInt(4000));
    _idleTimer = Timer(delay, () {
      if (!mounted) return;
      _idleFrom = _currentIdleOffset;
      _idleTo = Offset(
        (_random.nextDouble() * 2 - 1) * 0.08,
        (_random.nextDouble() * 2 - 1) * 0.05,
      );
      _idleController.forward(from: 0).then((_) {
        if (mounted) _scheduleNextIdleMove();
      });
    });
  }

  Offset get _currentIdleOffset => Offset.lerp(
    _idleFrom,
    _idleTo,
    Curves.easeInOutSine.transform(_idleController.value),
  )!;

  @override
  void dispose() {
    _idleTimer?.cancel();
    _idleController.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final eyeSize = widget.geometry.eyeRadius * 2;
    final gap = widget.geometry.eyeSpacing;
    return AnimatedBuilder(
      animation: _idleController,
      builder: (context, _) {
        final idleOffset = _currentIdleOffset;
        return Row(
          mainAxisSize: MainAxisSize.min,
          children: [
            _Eye(
              style: _styleFor(widget.expression),
              openness: widget.openness,
              size: eyeSize,
              mirrored: false,
              idleOffset: idleOffset,
            ),
            SizedBox(width: gap),
            _Eye(
              style: _styleFor(widget.expression),
              openness: widget.openness,
              size: eyeSize,
              mirrored: true,
              idleOffset: idleOffset,
            ),
          ],
        );
      },
    );
  }
}

@immutable
class _EyeStyle {
  const _EyeStyle({
    required this.scale,
    required this.pupilDx,
    required this.pupilDy,
    required this.color,
    required this.narrowed,
  });

  final double scale;

  /// Pupil offset within the eye, normalized roughly -0.3..0.3.
  final double pupilDx;
  final double pupilDy;
  final Color color;

  /// Happy eyes narrow into an upward arc instead of showing a round pupil.
  final bool narrowed;

  static _EyeStyle lerp(_EyeStyle a, _EyeStyle b, double t) {
    return _EyeStyle(
      scale: _lerpD(a.scale, b.scale, t),
      pupilDx: _lerpD(a.pupilDx, b.pupilDx, t),
      pupilDy: _lerpD(a.pupilDy, b.pupilDy, t),
      color: Color.lerp(a.color, b.color, t)!,
      narrowed: t < 0.5 ? a.narrowed : b.narrowed,
    );
  }
}

double _lerpD(double a, double b, double t) => a + (b - a) * t;

_EyeStyle _styleFor(RobotExpression expression) {
  switch (expression) {
    case RobotExpression.neutral:
      return const _EyeStyle(
        scale: 1,
        pupilDx: 0,
        pupilDy: 0,
        color: TthColors.faceCyan,
        narrowed: false,
      );
    case RobotExpression.listening:
      // Big, attentive, cheerful — "I'm listening carefully to you", not
      // blank or stiff. Bumped up from a subtler 1.15 for a clearly
      // bigger, more alert look, gaze centered.
      return const _EyeStyle(
        scale: 1.28,
        pupilDx: 0,
        pupilDy: 0,
        color: TthColors.faceCyan,
        narrowed: false,
      );
    case RobotExpression.thinking:
      return const _EyeStyle(
        scale: 0.95,
        pupilDx: 0.32,
        pupilDy: -0.32,
        color: TthColors.faceCyan,
        narrowed: false,
      );
    case RobotExpression.asking:
      // Wider attentive gaze, centered pupils.
      return const _EyeStyle(
        scale: 1.22,
        pupilDx: 0,
        pupilDy: -0.06,
        color: TthColors.faceCyan,
        narrowed: false,
      );
    case RobotExpression.speaking:
      return const _EyeStyle(
        scale: 1.05,
        pupilDx: 0,
        pupilDy: 0,
        color: TthColors.faceCyan,
        narrowed: false,
      );
    case RobotExpression.happy:
      return const _EyeStyle(
        scale: 1,
        pupilDx: 0,
        pupilDy: 0,
        color: TthColors.faceCyan,
        narrowed: true,
      );
    case RobotExpression.encouraging:
      return const _EyeStyle(
        scale: 0.9,
        pupilDx: 0,
        pupilDy: 0.08,
        color: TthColors.faceCyan,
        narrowed: false,
      );
    case RobotExpression.offline:
      return const _EyeStyle(
        scale: 0.55,
        pupilDx: 0,
        pupilDy: 0,
        color: TthColors.faceCyanDim,
        narrowed: false,
      );
  }
}

class _EyeStyleTween extends Tween<_EyeStyle> {
  _EyeStyleTween({required _EyeStyle begin, required _EyeStyle end})
    : super(begin: begin, end: end);

  @override
  _EyeStyle lerp(double t) => _EyeStyle.lerp(begin!, end!, t);
}

class _Eye extends StatelessWidget {
  const _Eye({
    required this.style,
    required this.openness,
    required this.size,
    required this.mirrored,
    required this.idleOffset,
  });

  final _EyeStyle style;
  final double openness;
  final double size;
  final bool mirrored;
  final Offset idleOffset;

  @override
  Widget build(BuildContext context) {
    return TweenAnimationBuilder<_EyeStyle>(
      tween: _EyeStyleTween(begin: style, end: style),
      duration: const Duration(milliseconds: 260),
      curve: Curves.easeInOutCubic,
      builder: (context, animatedStyle, _) {
        final height = size * animatedStyle.scale * openness.clamp(0.06, 1.0);
        final width = size * animatedStyle.scale;
        // Fixed headroom (kEyeOuterHeadroom) regardless of the animated
        // scale — sized to fit the largest expression scale in use
        // (listening, 1.28) with a small margin, so the eye never paints
        // outside its allotted space in the row. Shared with
        // RobotFaceGeometry, which relies on the exact same value to make
        // sure two eyes plus their gap actually fit the design width.
        return SizedBox(
          width: size * kEyeOuterHeadroom,
          height: size * kEyeOuterHeadroom,
          child: Center(
            child: SizedBox(
              width: width,
              height: height,
              child: CustomPaint(
                painter: _EyePainter(
                  style: animatedStyle,
                  openness: openness,
                  mirrored: mirrored,
                  idleOffset: idleOffset,
                ),
              ),
            ),
          ),
        );
      },
    );
  }
}

/// Layered eye rendering — outer glow, a sharp cyan ring, a darker "iris"
/// band, a pupil and a highlight (plus a fainter secondary reflection) —
/// rather than one flat filled shape, so the eye reads as premium/alive
/// per the design brief instead of "two circles".
class _EyePainter extends CustomPainter {
  const _EyePainter({
    required this.style,
    required this.openness,
    required this.mirrored,
    required this.idleOffset,
  });

  final _EyeStyle style;
  final double openness;
  final bool mirrored;
  final Offset idleOffset;

  static const Color _irisColor = Color(0xFF0C2430);
  static const Color _pupilColor = Color(0xFF04121A);

  @override
  void paint(Canvas canvas, Size size) {
    final center = Offset(size.width / 2, size.height / 2);
    final radius = size.shortestSide / 2;

    if (style.narrowed && openness > 0.5) {
      _paintHappyArc(canvas, size);
      return;
    }

    // 1. Subtle outer glow — kept tight (small blur radius) so the
    // contour stays sharp rather than a blurry neon smear.
    canvas.drawCircle(
      center,
      radius,
      Paint()
        ..color = style.color.withValues(alpha: 0.35)
        ..maskFilter = const MaskFilter.blur(BlurStyle.normal, 10),
    );

    // 2. Clean cyan outer ring.
    final ringWidth = radius * 0.2;
    canvas.drawCircle(
      center,
      radius - ringWidth / 2,
      Paint()
        ..style = PaintingStyle.stroke
        ..strokeWidth = ringWidth
        ..color = style.color,
    );

    // 3. Darker iris area, inset from the ring.
    final irisRadius = radius - ringWidth;
    canvas.drawCircle(center, irisRadius, Paint()..color = _irisColor);

    if (openness > 0.35) {
      // 4. Pupil, offset by the expression's gaze plus a slow idle drift.
      final pupilRadius = irisRadius * 0.58;
      final maxOffset = (irisRadius - pupilRadius).clamp(0.0, irisRadius);
      final dx = (mirrored ? -style.pupilDx : style.pupilDx) + idleOffset.dx;
      final dy = style.pupilDy + idleOffset.dy;
      final rawOffset = Offset(dx, dy) * maxOffset * 1.4;
      final distance = rawOffset.distance;
      final pupilCenter = distance > maxOffset && distance > 0
          ? center + rawOffset * (maxOffset / distance)
          : center + rawOffset;

      canvas.drawCircle(pupilCenter, pupilRadius, Paint()..color = _pupilColor);

      // 5. Bright primary highlight, upper area of the pupil.
      canvas.drawCircle(
        pupilCenter + Offset(pupilRadius * 0.08, -pupilRadius * 0.4),
        pupilRadius * 0.32,
        Paint()..color = Colors.white,
      );
      // 6. Smaller secondary reflection, clustered near the primary one
      // (not the opposite corner) — matches the reference's "glossy eye"
      // look, where both highlights sit together in the upper area.
      canvas.drawCircle(
        pupilCenter + Offset(-pupilRadius * 0.32, -pupilRadius * 0.02),
        pupilRadius * 0.15,
        Paint()..color = Colors.white.withValues(alpha: 0.85),
      );
    }
  }

  void _paintHappyArc(Canvas canvas, Size size) {
    final glow = Paint()
      ..color = style.color.withValues(alpha: 0.35)
      ..maskFilter = const MaskFilter.blur(BlurStyle.normal, 10);
    final paint = Paint()
      ..color = style.color
      ..style = PaintingStyle.stroke
      ..strokeWidth = size.shortestSide * 0.26
      ..strokeCap = StrokeCap.round;
    final rect = Rect.fromLTWH(
      0,
      -size.height * 0.3,
      size.width,
      size.height * 1.6,
    );
    const startAngle = 3.4; // radians, upper arc opening downward
    const sweep = 2.2;
    canvas.drawArc(rect.inflate(6), startAngle, sweep, false, glow);
    canvas.drawArc(rect, startAngle, sweep, false, paint);
  }

  @override
  bool shouldRepaint(covariant _EyePainter oldDelegate) =>
      oldDelegate.style != style ||
      oldDelegate.openness != openness ||
      oldDelegate.mirrored != mirrored ||
      oldDelegate.idleOffset != idleOffset;
}
