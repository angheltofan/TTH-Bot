import 'package:flutter_test/flutter_test.dart';
import 'package:tth_bot/features/activities/activity.dart';

void main() {
  group('ActivityType <-> db string', () {
    test('round-trips every value', () {
      for (final type in ActivityType.values) {
        expect(ActivityTypeDb.fromDbValue(type.dbValue), type);
      }
    });

    test('uses the exact strings the `type` check constraint allows', () {
      expect(ActivityType.lesson.dbValue, 'lesson');
      expect(ActivityType.conversation.dbValue, 'conversation');
      expect(ActivityType.game.dbValue, 'game');
    });

    test('throws FormatException for an unknown db value', () {
      expect(() => ActivityTypeDb.fromDbValue('quiz'), throwsFormatException);
    });
  });

  group('InteractionMode <-> db string', () {
    test('round-trips every value', () {
      for (final mode in InteractionMode.values) {
        expect(InteractionModeDb.fromDbValue(mode.dbValue), mode);
      }
    });

    test(
      'uses the exact strings the `interaction_mode` check constraint allows',
      () {
        expect(InteractionMode.pushToTalk.dbValue, 'push_to_talk');
        expect(InteractionMode.freeConversation.dbValue, 'free_conversation');
      },
    );

    test('throws FormatException for an unknown db value', () {
      expect(
        () => InteractionModeDb.fromDbValue('telepathy'),
        throwsFormatException,
      );
    });
  });

  group('Activity.fromMap', () {
    test('parses a full Supabase row', () {
      final activity = Activity.fromMap({
        'id': 'abc-123',
        'title': 'Animale',
        'type': 'game',
        'prompt': 'Joacă un joc de ghicitori despre animale.',
        'participants': ['Ana', 'Matei'],
        'interaction_mode': 'push_to_talk',
        'enabled': true,
        'sort_order': 2,
        'created_at': '2026-08-12T10:00:00Z',
        'updated_at': '2026-08-12T10:05:00Z',
      });

      expect(activity.id, 'abc-123');
      expect(activity.title, 'Animale');
      expect(activity.type, ActivityType.game);
      expect(
        activity.systemPrompt,
        'Joacă un joc de ghicitori despre animale.',
      );
      expect(activity.participants, ['Ana', 'Matei']);
      expect(activity.interactionMode, InteractionMode.pushToTalk);
      expect(activity.enabled, isTrue);
      expect(activity.sortOrder, 2);
      expect(activity.createdAt, DateTime.parse('2026-08-12T10:00:00Z'));
      expect(activity.updatedAt, DateTime.parse('2026-08-12T10:05:00Z'));
    });

    test('defaults participants/enabled/sortOrder when absent', () {
      final activity = Activity.fromMap({
        'id': 'x',
        'title': 'Minimal',
        'type': 'lesson',
        'prompt': 'p',
        'interaction_mode': 'push_to_talk',
      });

      expect(activity.participants, isEmpty);
      expect(activity.enabled, isTrue);
      expect(activity.sortOrder, 0);
      expect(activity.createdAt, isNull);
      expect(activity.updatedAt, isNull);
    });
  });

  group('write payloads', () {
    const activity = Activity(
      id: 'should-be-omitted',
      title: 'Animale',
      type: ActivityType.game,
      systemPrompt: 'Joacă un joc de ghicitori.',
      interactionMode: InteractionMode.pushToTalk,
      participants: ['Ana', 'Matei'],
      enabled: false,
      sortOrder: 3,
    );

    test('toInsertMap omits id and timestamps', () {
      final map = activity.toInsertMap();

      expect(map.containsKey('id'), isFalse);
      expect(map.containsKey('created_at'), isFalse);
      expect(map.containsKey('updated_at'), isFalse);
      expect(map['title'], 'Animale');
      expect(map['type'], 'game');
      expect(map['prompt'], 'Joacă un joc de ghicitori.');
      expect(map['participants'], ['Ana', 'Matei']);
      expect(map['interaction_mode'], 'push_to_talk');
      expect(map['enabled'], isFalse);
      expect(map['sort_order'], 3);
    });

    test('toUpdateMap matches toInsertMap (id is a filter, not a column)', () {
      expect(activity.toUpdateMap(), activity.toInsertMap());
    });
  });

  test('fromMap(toMap()) round-trips a full activity', () {
    final original = Activity(
      id: 'abc',
      title: 'Baschet',
      type: ActivityType.lesson,
      systemPrompt: 'p',
      interactionMode: InteractionMode.pushToTalk,
      participants: const ['Maria', 'Sofia'],
      enabled: true,
      sortOrder: 5,
      createdAt: DateTime.utc(2026, 8, 12),
      updatedAt: DateTime.utc(2026, 8, 12, 1),
    );

    final roundTripped = Activity.fromMap(original.toMap());

    expect(roundTripped.id, original.id);
    expect(roundTripped.title, original.title);
    expect(roundTripped.type, original.type);
    expect(roundTripped.systemPrompt, original.systemPrompt);
    expect(roundTripped.interactionMode, original.interactionMode);
    expect(roundTripped.participants, original.participants);
    expect(roundTripped.enabled, original.enabled);
    expect(roundTripped.sortOrder, original.sortOrder);
    expect(roundTripped.createdAt, original.createdAt);
    expect(roundTripped.updatedAt, original.updatedAt);
  });
}
