import 'package:flutter_test/flutter_test.dart';
import 'package:tth_bot/features/robot/robot_expression.dart';
import 'package:tth_bot/features/voice/voice_session_controller.dart';

void main() {
  group('mapConversationStateToExpression', () {
    test('disconnected and error map to neutral, not offline — the face must '
        'not visibly change into a "standby" look just because the session '
        'is idle/dropped between uses', () {
      expect(
        mapConversationStateToExpression(ConversationState.disconnected),
        RobotExpression.neutral,
      );
      expect(
        mapConversationStateToExpression(ConversationState.error),
        RobotExpression.neutral,
      );
    });

    test('connecting and ready map to neutral', () {
      expect(
        mapConversationStateToExpression(ConversationState.connecting),
        RobotExpression.neutral,
      );
      expect(
        mapConversationStateToExpression(ConversationState.ready),
        RobotExpression.neutral,
      );
    });

    test('listening maps to listening', () {
      expect(
        mapConversationStateToExpression(ConversationState.listening),
        RobotExpression.listening,
      );
    });

    test('waiting (for the AI) maps to thinking', () {
      expect(
        mapConversationStateToExpression(ConversationState.waiting),
        RobotExpression.thinking,
      );
    });

    test('speaking maps to speaking', () {
      expect(
        mapConversationStateToExpression(ConversationState.speaking),
        RobotExpression.speaking,
      );
    });

    test('every ConversationState value has a mapping (exhaustive)', () {
      for (final state in ConversationState.values) {
        expect(() => mapConversationStateToExpression(state), returnsNormally);
      }
    });
  });
}
