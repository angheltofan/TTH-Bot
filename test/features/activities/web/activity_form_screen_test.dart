import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tth_bot/features/activities/activity.dart';
import 'package:tth_bot/features/activities/activity_repository.dart';
import 'package:tth_bot/features/activities/web/activity_form_screen.dart';

class _FakeActivityRepository implements ActivityRepository {
  Activity? created;
  Activity? updated;
  Object? saveError;

  @override
  Future<List<Activity>> getEnabledActivities() async => const [];

  @override
  Future<List<Activity>> getAllActivities() async => const [];

  @override
  Future<Activity> getActivity(String id) async => throw UnimplementedError();

  @override
  Future<Activity> createActivity(Activity activity) async {
    if (saveError != null) throw saveError!;
    created = activity;
    return activity;
  }

  @override
  Future<Activity> updateActivity(Activity activity) async {
    if (saveError != null) throw saveError!;
    updated = activity;
    return activity;
  }

  @override
  Future<void> deleteActivity(String id) async {}
}

const _existing = Activity(
  id: 'baschet',
  title: 'Baschet',
  type: ActivityType.lesson,
  systemPrompt: 'Explică baschetul.',
  interactionMode: InteractionMode.pushToTalk,
  participants: ['Maria', 'Sofia'],
  sortOrder: 0,
);

/// Mutable holder so the test can read the pushed route's pop value after
/// it resolves (which happens later than [_pump] itself returns — only
/// once the form screen actually pops).
class _PopResult {
  Object? value;
}

Future<void> _pump(
  WidgetTester tester, {
  required _FakeActivityRepository repository,
  Activity? existing,
  _PopResult? popResult,
}) async {
  await tester.pumpWidget(
    MaterialApp(
      home: Builder(
        builder: (context) => Scaffold(
          body: Center(
            child: ElevatedButton(
              onPressed: () async {
                final result = await Navigator.of(context).push<Object?>(
                  MaterialPageRoute<Object?>(
                    builder: (_) => ActivityFormScreen(
                      repository: repository,
                      existing: existing,
                    ),
                  ),
                );
                popResult?.value = result;
              },
              child: const Text('open'),
            ),
          ),
        ),
      ),
    ),
  );
  await tester.tap(find.text('open'));
  await tester.pumpAndSettle();
}

/// The form is a [SingleChildScrollView] taller than the default test
/// viewport, so the Save button can be off-screen — scroll it into view
/// before tapping.
Future<void> _tapSave(WidgetTester tester) async {
  await tester.ensureVisible(find.text('Salvează'));
  await tester.pumpAndSettle();
  await tester.tap(find.text('Salvează'));
  await tester.pumpAndSettle();
}

void main() {
  testWidgets('rejects an empty title without saving', (tester) async {
    final repository = _FakeActivityRepository();
    await _pump(tester, repository: repository);

    await _tapSave(tester);

    expect(find.text('Titlul este obligatoriu'), findsOneWidget);
    expect(repository.created, isNull);
  });

  testWidgets('rejects a non-integer sort order without saving', (
    tester,
  ) async {
    final repository = _FakeActivityRepository();
    await _pump(tester, repository: repository);

    await tester.enterText(find.widgetWithText(TextFormField, 'Titlu'), 'X');
    await tester.enterText(
      find.widgetWithText(TextFormField, 'Prompt'),
      'Un prompt.',
    );
    await tester.enterText(find.widgetWithText(TextFormField, 'Ordine'), 'abc');
    await _tapSave(tester);

    expect(find.text('Introdu un număr întreg'), findsOneWidget);
    expect(repository.created, isNull);
  });

  testWidgets(
    'creating a new activity parses participants in order and pops true',
    (tester) async {
      final repository = _FakeActivityRepository();
      final popResult = _PopResult();
      await _pump(tester, repository: repository, popResult: popResult);

      await tester.enterText(
        find.widgetWithText(TextFormField, 'Titlu'),
        'Animale',
      );
      await tester.enterText(
        find.widgetWithText(TextFormField, 'Prompt'),
        'Joacă un joc de ghicitori despre animale.',
      );
      await tester.enterText(
        find.widgetWithText(TextFormField, 'Copiii participanți'),
        'Ana, Matei',
      );
      await _tapSave(tester);

      expect(repository.created, isNotNull);
      expect(repository.created!.title, 'Animale');
      expect(repository.created!.participants, ['Ana', 'Matei']);
      expect(repository.created!.enabled, isTrue);
      expect(popResult.value, isTrue);
    },
  );

  testWidgets('editing an existing activity pre-fills fields and updates', (
    tester,
  ) async {
    final repository = _FakeActivityRepository();
    await _pump(tester, repository: repository, existing: _existing);

    expect(find.text('Baschet'), findsOneWidget);
    expect(find.text('Maria, Sofia'), findsOneWidget);

    await _tapSave(tester);

    expect(repository.updated, isNotNull);
    expect(repository.updated!.id, 'baschet');
    expect(repository.updated!.title, 'Baschet');
  });

  testWidgets(
    'shows a save error instead of popping when the repository fails',
    (tester) async {
      final repository = _FakeActivityRepository()
        ..saveError = Exception('network down');
      await _pump(tester, repository: repository, existing: _existing);

      await _tapSave(tester);

      expect(find.textContaining('Eroare la salvare'), findsOneWidget);
    },
  );
}
