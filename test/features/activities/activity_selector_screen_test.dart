import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tth_bot/features/activities/activity.dart';
import 'package:tth_bot/features/activities/activity_repository.dart';
import 'package:tth_bot/features/activities/activity_selector_screen.dart';

const _basketball = Activity(
  id: 'baschet',
  title: 'Baschet',
  type: ActivityType.lesson,
  systemPrompt: 'x',
  interactionMode: InteractionMode.pushToTalk,
  participants: ['Maria', 'Sofia', 'Alex', 'Ștefan'],
);

const _freeChat = Activity(
  id: 'conversatie-libera',
  title: 'Conversație liberă',
  type: ActivityType.conversation,
  systemPrompt: 'y',
  interactionMode: InteractionMode.pushToTalk,
);

class _FakeActivityRepository implements ActivityRepository {
  _FakeActivityRepository({this.error});

  List<Activity> enabled = [_basketball, _freeChat];
  Object? error;
  int getEnabledActivitiesCallCount = 0;

  /// When set, [getEnabledActivities] waits on this instead of resolving
  /// immediately — lets a test pump a frame *while* a load is in flight.
  Completer<void>? pendingGate;

  @override
  Future<List<Activity>> getEnabledActivities() async {
    getEnabledActivitiesCallCount++;
    if (pendingGate != null) await pendingGate!.future;
    if (error != null) throw error!;
    return enabled;
  }

  @override
  Future<List<Activity>> getAllActivities() async => enabled;

  @override
  Future<Activity> getActivity(String id) async =>
      enabled.firstWhere((a) => a.id == id);

  @override
  Future<Activity> createActivity(Activity activity) async => activity;

  @override
  Future<Activity> updateActivity(Activity activity) async => activity;

  @override
  Future<void> deleteActivity(String id) async {}
}

void main() {
  testWidgets('shows both activity titles once loaded', (tester) async {
    await tester.pumpWidget(
      MaterialApp(
        home: ActivitySelectorScreen(
          repository: _FakeActivityRepository(),
          onActivitySelected: (_) async {},
        ),
      ),
    );
    await tester.pumpAndSettle();

    expect(find.text('Baschet'), findsOneWidget);
    expect(find.text('Conversație liberă'), findsOneWidget);
  });

  testWidgets(
    'tapping an activity reports it via the callback, without navigating '
    'on its own',
    (tester) async {
      Activity? selected;

      await tester.pumpWidget(
        MaterialApp(
          home: ActivitySelectorScreen(
            repository: _FakeActivityRepository(),
            onActivitySelected: (activity) async => selected = activity,
          ),
        ),
      );
      await tester.pumpAndSettle();

      await tester.tap(find.text('Baschet'));
      await tester.pumpAndSettle();

      expect(selected, _basketball);
    },
  );

  testWidgets('reloads after returning from the selected activity', (
    tester,
  ) async {
    final repository = _FakeActivityRepository();

    await tester.pumpWidget(
      MaterialApp(
        home: ActivitySelectorScreen(
          repository: repository,
          onActivitySelected: (_) async {
            // Simulate the pushed robot-face screen having already
            // returned by the time this resolves.
          },
        ),
      ),
    );
    await tester.pumpAndSettle();
    expect(repository.getEnabledActivitiesCallCount, 1);

    await tester.tap(find.text('Baschet'));
    await tester.pumpAndSettle();

    expect(repository.getEnabledActivitiesCallCount, 2);
  });

  testWidgets('a failed load shows a retry button, and retry reloads', (
    tester,
  ) async {
    final repository = _FakeActivityRepository(error: Exception('offline'));

    await tester.pumpWidget(
      MaterialApp(
        home: ActivitySelectorScreen(
          repository: repository,
          onActivitySelected: (_) async {},
        ),
      ),
    );
    await tester.pumpAndSettle();

    expect(find.text('Baschet'), findsNothing);
    expect(find.text('Încearcă din nou'), findsOneWidget);

    repository.error = null;
    await tester.tap(find.text('Încearcă din nou'));
    await tester.pumpAndSettle();

    expect(find.text('Baschet'), findsOneWidget);
  });

  testWidgets(
    'initial load with no cache yet shows the blocking spinner, not the list',
    (tester) async {
      final repository = _FakeActivityRepository()
        ..pendingGate = Completer<void>();

      await tester.pumpWidget(
        MaterialApp(
          home: ActivitySelectorScreen(
            repository: repository,
            onActivitySelected: (_) async {},
          ),
        ),
      );
      await tester.pump();

      expect(find.byType(CircularProgressIndicator), findsOneWidget);
      expect(find.text('Baschet'), findsNothing);

      repository.pendingGate!.complete();
      await tester.pumpAndSettle();
      expect(find.text('Baschet'), findsOneWidget);
    },
  );

  testWidgets(
    'returning from an activity keeps showing cached activities while '
    'refreshing in the background, instead of a blocking spinner',
    (tester) async {
      final repository = _FakeActivityRepository();

      await tester.pumpWidget(
        MaterialApp(
          home: ActivitySelectorScreen(
            repository: repository,
            onActivitySelected: (_) async {},
          ),
        ),
      );
      await tester.pumpAndSettle();
      expect(find.text('Baschet'), findsOneWidget);

      // The next load (triggered by "returning" from the tapped activity)
      // is gated so we can observe the screen mid-refresh.
      repository.pendingGate = Completer<void>();
      await tester.tap(find.text('Baschet'));
      await tester.pump();

      // Still showing the cached list, no full-screen spinner — only the
      // small non-blocking refresh indicator.
      expect(find.text('Baschet'), findsOneWidget);
      expect(find.byType(CircularProgressIndicator), findsOneWidget);

      repository.pendingGate!.complete();
      await tester.pumpAndSettle();
      expect(find.text('Baschet'), findsOneWidget);
    },
  );

  testWidgets('a background refresh updates the visible activity list', (
    tester,
  ) async {
    final repository = _FakeActivityRepository();

    await tester.pumpWidget(
      MaterialApp(
        home: ActivitySelectorScreen(
          repository: repository,
          onActivitySelected: (_) async {},
        ),
      ),
    );
    await tester.pumpAndSettle();
    expect(find.text('Baschet'), findsOneWidget);
    expect(find.text('Conversație liberă'), findsOneWidget);

    // Simulate an edit on the web: "Conversație liberă" got disabled.
    repository.enabled = [_basketball];
    await tester.tap(find.text('Baschet'));
    await tester.pumpAndSettle();

    expect(find.text('Baschet'), findsOneWidget);
    expect(find.text('Conversație liberă'), findsNothing);
  });

  testWidgets('a failed background refresh keeps the cached activities visible '
      'instead of showing the error view', (tester) async {
    final repository = _FakeActivityRepository();

    await tester.pumpWidget(
      MaterialApp(
        home: ActivitySelectorScreen(
          repository: repository,
          onActivitySelected: (_) async {},
        ),
      ),
    );
    await tester.pumpAndSettle();
    expect(find.text('Baschet'), findsOneWidget);

    repository.error = Exception('offline for the refresh');
    await tester.tap(find.text('Baschet'));
    await tester.pumpAndSettle();

    // Still the cached list — not the full-screen error view.
    expect(find.text('Baschet'), findsOneWidget);
    expect(find.text('Încearcă din nou'), findsNothing);
    expect(find.text('Nu am putut încărca activitățile.'), findsNothing);
  });
}
