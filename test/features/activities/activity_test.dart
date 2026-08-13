import 'package:flutter_test/flutter_test.dart';
import 'package:tth_bot/features/activities/activity.dart';
import 'package:tth_bot/features/activities/activity_repository.dart';
import 'package:tth_bot/features/activities/hardcoded_activities.dart';

void main() {
  group('Activity model', () {
    test('holds the fields it is given', () {
      const activity = Activity(
        id: 'test-id',
        title: 'Test',
        type: ActivityType.lesson,
        systemPrompt: 'do the test thing',
        interactionMode: InteractionMode.pushToTalk,
        participants: ['A', 'B'],
      );

      expect(activity.id, 'test-id');
      expect(activity.title, 'Test');
      expect(activity.type, ActivityType.lesson);
      expect(activity.systemPrompt, 'do the test thing');
      expect(activity.interactionMode, InteractionMode.pushToTalk);
      expect(activity.participants, ['A', 'B']);
    });

    test('defaults to no participants', () {
      const activity = Activity(
        id: 'x',
        title: 'X',
        type: ActivityType.conversation,
        systemPrompt: 'chat',
        interactionMode: InteractionMode.pushToTalk,
      );

      expect(activity.participants, isEmpty);
    });
  });

  group('basketballActivity', () {
    test('addresses the four children in the exact required order', () {
      expect(basketballActivity.participants, [
        'Maria',
        'Sofia',
        'Alex',
        'Ștefan',
      ]);
    });

    test('is a push-to-talk lesson', () {
      expect(basketballActivity.type, ActivityType.lesson);
      expect(basketballActivity.interactionMode, InteractionMode.pushToTalk);
    });

    test('system prompt mentions the key basketball concepts', () {
      final prompt = basketballActivity.systemPrompt.toLowerCase();
      for (final concept in [
        'echipă',
        'coș',
        'pasa',
        'dribling',
        'precizie',
        'reguli',
        'antrenament',
      ]) {
        expect(
          prompt.contains(concept),
          isTrue,
          reason: 'expected basketball prompt to mention "$concept"',
        );
      }
    });

    test('system prompt asks for a richer 60-90s, 6-8 idea introduction', () {
      final prompt = basketballActivity.systemPrompt;
      expect(prompt, contains('60-90'));
      expect(prompt, contains('6-8'));
    });

    test('system prompt instructs one question at a time, not all at once', () {
      final prompt = basketballActivity.systemPrompt;
      expect(prompt, contains('o singură întrebare'));
      expect(prompt, contains('Nu trece la următorul copil'));
    });
  });

  group('freeConversationActivity', () {
    test('has no fixed participants and is genuinely hands-free', () {
      expect(freeConversationActivity.participants, isEmpty);
      expect(freeConversationActivity.type, ActivityType.conversation);
      expect(
        freeConversationActivity.interactionMode,
        InteractionMode.freeConversation,
      );
    });
  });

  group('HardcodedActivityRepository', () {
    const repository = HardcodedActivityRepository();

    test(
      'getAllActivities returns Baschet and Conversație liberă in order',
      () async {
        final activities = await repository.getAllActivities();

        expect(activities, [basketballActivity, freeConversationActivity]);
      },
    );

    test(
      'getEnabledActivities returns the same two (both enabled by default)',
      () async {
        final activities = await repository.getEnabledActivities();

        expect(activities, [basketballActivity, freeConversationActivity]);
      },
    );

    test('getActivity finds a known id and rejects an unknown one', () async {
      final activity = await repository.getActivity('baschet');

      expect(activity, basketballActivity);
      await expectLater(
        () => repository.getActivity('does-not-exist'),
        throwsStateError,
      );
    });

    test('is read-only: create/update/delete all throw', () async {
      await expectLater(
        () => repository.createActivity(basketballActivity),
        throwsUnsupportedError,
      );
      await expectLater(
        () => repository.updateActivity(basketballActivity),
        throwsUnsupportedError,
      );
      await expectLater(
        () => repository.deleteActivity('baschet'),
        throwsUnsupportedError,
      );
    });
  });
}
