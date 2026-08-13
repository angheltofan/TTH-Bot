import 'package:flutter_test/flutter_test.dart';
import 'package:tth_bot/core/prompts/edubot_base_prompt.dart';
import 'package:tth_bot/features/activities/activity.dart';
import 'package:tth_bot/features/activities/prompt_composer.dart';

void main() {
  group('composeSystemInstruction', () {
    const activityWithParticipants = Activity(
      id: 'a',
      title: 'A',
      type: ActivityType.lesson,
      systemPrompt: 'ACTIVITY SPECIFIC TEXT',
      interactionMode: InteractionMode.pushToTalk,
      participants: ['Ana', 'Bogdan'],
    );

    const activityWithoutParticipants = Activity(
      id: 'b',
      title: 'B',
      type: ActivityType.conversation,
      systemPrompt: 'FREE CHAT TEXT',
      interactionMode: InteractionMode.pushToTalk,
    );

    test('includes the base prompt', () {
      final result = composeSystemInstruction(activityWithParticipants);

      expect(result, contains(eduBotBasePrompt.trim()));
    });

    test('includes the activity-specific prompt', () {
      final result = composeSystemInstruction(activityWithParticipants);

      expect(result, contains('ACTIVITY SPECIFIC TEXT'));
    });

    test('base prompt comes before the activity prompt', () {
      final result = composeSystemInstruction(activityWithParticipants);

      final baseIndex = result.indexOf(eduBotBasePrompt.trim());
      final activityIndex = result.indexOf('ACTIVITY SPECIFIC TEXT');

      expect(baseIndex, greaterThanOrEqualTo(0));
      expect(activityIndex, greaterThan(baseIndex));
    });

    test('appends participants in order when present', () {
      final result = composeSystemInstruction(activityWithParticipants);

      expect(result, contains('Ana, Bogdan'));
      expect(
        result.indexOf('Ana, Bogdan'),
        greaterThan(result.indexOf('ACTIVITY SPECIFIC TEXT')),
      );
    });

    test('adds no participant section when there are none', () {
      final result = composeSystemInstruction(activityWithoutParticipants);

      expect(result, isNot(contains('participanți')));
    });
  });
}
