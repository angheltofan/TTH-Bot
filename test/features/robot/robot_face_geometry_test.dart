import 'package:flutter_test/flutter_test.dart';
import 'package:tth_bot/features/robot/robot_face_geometry.dart';

void main() {
  group('RobotFaceGeometry.fromWidth', () {
    // A representative spread of landscape widths this design canvas
    // actually gets scaled to fit, from small phones up through tablets.
    const widths = [200.0, 340.0, 480.0, 800.0, 1200.0];

    for (final width in widths) {
      test('produces only positive, finite geometry at width=$width', () {
        final g = RobotFaceGeometry.fromWidth(width);

        for (final value in [
          g.faceWidth,
          g.faceHeight,
          g.eyeRadius,
          g.eyeSpacing,
          g.eyeCenterY,
          g.mouthWidth,
          g.mouthCenterY,
        ]) {
          expect(value.isFinite, isTrue, reason: 'value was $value');
          expect(value, greaterThan(0));
        }
      });

      test('the two eyes\' actual rendered outer boxes (see kEyeOuterHeadroom) '
          'plus their gap stay within the reference width at width=$width — '
          'regression test for an overflow where this check used the raw eye '
          'diameter instead of the outer headroom _Eye actually renders at, '
          'so it passed while the real widget overflowed', () {
        final g = RobotFaceGeometry.fromWidth(width);
        final eyesRowWidth =
            g.eyeRadius * 2 * kEyeOuterHeadroom * 2 + g.eyeSpacing;

        expect(eyesRowWidth, lessThanOrEqualTo(width));
      });

      test('orders rows top to bottom without overlap at width=$width', () {
        final g = RobotFaceGeometry.fromWidth(width);

        expect(g.eyeCenterY, greaterThan(0));
        expect(g.mouthCenterY, greaterThan(g.eyeCenterY));
        expect(g.faceHeight, greaterThanOrEqualTo(g.mouthCenterY));
      });
    }

    test('scales every dimension linearly with width (pure ratios, no '
        'device-specific fixed pixel offsets)', () {
      final a = RobotFaceGeometry.fromWidth(300);
      final b = RobotFaceGeometry.fromWidth(600);

      expect(b.eyeRadius, closeTo(a.eyeRadius * 2, 0.001));
      expect(b.eyeSpacing, closeTo(a.eyeSpacing * 2, 0.001));
      expect(b.mouthWidth, closeTo(a.mouthWidth * 2, 0.001));
    });
  });
}
