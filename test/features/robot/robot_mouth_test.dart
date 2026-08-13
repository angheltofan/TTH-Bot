import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tth_bot/features/robot/robot_expression.dart';
import 'package:tth_bot/features/robot/robot_mouth.dart';

void main() {
  testWidgets('renders without error for every expression', (tester) async {
    final amplitude = ValueNotifier<double>(0);
    for (final expression in RobotExpression.values) {
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: RobotMouth(
              expression: expression,
              amplitude: amplitude,
              width: 100,
            ),
          ),
        ),
      );
      await tester.pump(const Duration(milliseconds: 300));
      expect(tester.takeException(), isNull);
    }
  });

  testWidgets(
    'while speaking, mouth amplitude changes redraw without rebuilding the '
    'whole mouth widget (the ValueKey seam that keeps AnimatedSwitcher from '
    'restarting on every audio tick)',
    (tester) async {
      final amplitude = ValueNotifier<double>(0);
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: RobotMouth(
              expression: RobotExpression.speaking,
              amplitude: amplitude,
              width: 100,
            ),
          ),
        ),
      );
      await tester.pump();

      final before = find.byKey(const ValueKey('speaking'));
      expect(before, findsOneWidget);

      amplitude.value = 0.8;
      await tester.pump();
      expect(tester.takeException(), isNull);
      expect(find.byKey(const ValueKey('speaking')), findsOneWidget);

      amplitude.value = 0.1;
      await tester.pump();
      expect(tester.takeException(), isNull);
    },
  );
}
