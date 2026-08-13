import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tth_bot/features/robot/robot_expression.dart';
import 'package:tth_bot/features/robot/robot_face.dart';

void main() {
  testWidgets('renders every expression without a layout overflow, at the real '
      'design width — regression test for the eye-sizing overflow a '
      'geometry-only unit test failed to catch', (tester) async {
    final amplitude = ValueNotifier<double>(0);
    for (final expression in RobotExpression.values) {
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: RobotFace(expression: expression, amplitude: amplitude),
          ),
        ),
      );
      await tester.pump(const Duration(milliseconds: 300));
      expect(tester.takeException(), isNull, reason: 'expression: $expression');
    }
  });

  testWidgets(
    'renders without overflow on a real landscape-phone aspect ratio',
    (tester) async {
      tester.view.physicalSize = const Size(2400, 1080);
      tester.view.devicePixelRatio = 3.0;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);

      final amplitude = ValueNotifier<double>(0);
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: RobotFace(
              expression: RobotExpression.listening,
              amplitude: amplitude,
            ),
          ),
        ),
      );
      await tester.pump(const Duration(milliseconds: 300));

      expect(tester.takeException(), isNull);
    },
  );
}
