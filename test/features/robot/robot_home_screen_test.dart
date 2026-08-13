import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tth_bot/features/activities/activity.dart';
import 'package:tth_bot/features/activities/activity_repository.dart';
import 'package:tth_bot/features/robot/robot_home_screen.dart';
import 'package:tth_bot/features/voice/gemini_auth.dart';

// Deliberately push-to-talk-only fixtures: selecting a free-conversation
// activity makes VoiceSessionController dial a real Gemini Live connection
// (RobotHomeScreen doesn't inject fake voice services), which would hit the
// network from a widget test. Push-to-talk only connects on a press, which
// these tests never simulate, so construction alone stays side-effect-free.
const _basketball = Activity(
  id: 'baschet',
  title: 'Baschet',
  type: ActivityType.lesson,
  systemPrompt: 'x',
  interactionMode: InteractionMode.pushToTalk,
  sortOrder: 5,
);

const _riddles = Activity(
  id: 'ghicitori',
  title: 'Ghicitori',
  type: ActivityType.game,
  systemPrompt: 'y',
  interactionMode: InteractionMode.pushToTalk,
  sortOrder: 1,
);

// Stands in for an activity created on the web *after* app startup — the
// exact reproduction case for the stale-parent-cache bug (see the tests
// below named after it).
const _testActivitate = Activity(
  id: 'test-activitate',
  title: 'Test Activitate',
  type: ActivityType.lesson,
  systemPrompt: 'z',
  interactionMode: InteractionMode.pushToTalk,
  sortOrder: 10,
);

class _FakeActivityRepository implements ActivityRepository {
  _FakeActivityRepository(this.enabled);

  List<Activity> enabled;
  Object? error;
  int callCount = 0;

  @override
  Future<List<Activity>> getEnabledActivities() async {
    callCount++;
    if (error != null) throw error!;
    final sorted = List<Activity>.of(enabled)
      ..sort((a, b) => a.sortOrder.compareTo(b.sortOrder));
    return sorted;
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

Future<GeminiAuth> _fakeResolveAuth() async =>
    const GeminiApiKeyAuth('test-key');

Future<void> _pump(WidgetTester tester, ActivityRepository repository) async {
  await tester.pumpWidget(
    MaterialApp(
      home: RobotHomeScreen(
        repository: repository,
        resolveAuth: _fakeResolveAuth,
      ),
    ),
  );
  await tester.pumpAndSettle();
}

/// Opens the developer overlay to read which activity is current — the
/// only observable window into RobotHomeScreen's private state. Reachable
/// only by long-pressing the settings gear (debug builds only — `flutter
/// test` runs in debug mode, so kDebugMode is true here), never a
/// permanently visible affordance — see the Phase 5 remediation's "no
/// permanent debug panel" requirement.
Future<void> _openDebugOverlay(WidgetTester tester) async {
  await tester.longPress(find.byIcon(Icons.settings));
  await tester.pumpAndSettle();
}

void main() {
  testWidgets('starts directly on the robot face, no full-screen selector', (
    tester,
  ) async {
    final repository = _FakeActivityRepository([_basketball, _riddles]);
    await _pump(tester, repository);

    expect(find.text('Alege o activitate:'), findsNothing);
    expect(find.text('Baschet'), findsNothing);
    expect(find.text('Ghicitori'), findsNothing);
  });

  testWidgets('the first enabled activity by sort_order becomes the default', (
    tester,
  ) async {
    final repository = _FakeActivityRepository([_basketball, _riddles]);
    await _pump(tester, repository);

    await _openDebugOverlay(tester);

    // Ghicitori has sort_order 1, Baschet has sort_order 5.
    expect(find.textContaining('Activitate: Ghicitori'), findsOneWidget);
  });

  testWidgets('the settings gear opens a compact modal listing enabled '
      'activities', (tester) async {
    final repository = _FakeActivityRepository([_basketball, _riddles]);
    await _pump(tester, repository);

    await tester.tap(find.byIcon(Icons.settings));
    await tester.pumpAndSettle();

    expect(find.text('Activitate'), findsOneWidget);
    expect(find.text('Baschet'), findsOneWidget);
    expect(find.text('Ghicitori'), findsOneWidget);
  });

  testWidgets('selecting a different activity in the modal switches the '
      'current activity and closes the modal', (tester) async {
    final repository = _FakeActivityRepository([_basketball, _riddles]);
    await _pump(tester, repository);
    await _openDebugOverlay(tester);
    expect(find.textContaining('Activitate: Ghicitori'), findsOneWidget);

    await tester.tap(find.byIcon(Icons.settings));
    await tester.pumpAndSettle();
    await tester.tap(find.text('Baschet'));
    await tester.pumpAndSettle();

    // Modal is gone...
    expect(find.text('Activitate'), findsNothing);
    // ...and the debug overlay (still open underneath) reflects the switch.
    expect(find.textContaining('Activitate: Baschet'), findsOneWidget);
  });

  testWidgets('closing the modal without picking a new activity keeps the '
      'current one', (tester) async {
    final repository = _FakeActivityRepository([_basketball, _riddles]);
    await _pump(tester, repository);
    await _openDebugOverlay(tester);

    await tester.tap(find.byIcon(Icons.settings));
    await tester.pumpAndSettle();
    await tester.tap(find.text('Închide'));
    await tester.pumpAndSettle();

    expect(find.textContaining('Activitate: Ghicitori'), findsOneWidget);
  });

  testWidgets('refresh inside the modal updates the activity list', (
    tester,
  ) async {
    final repository = _FakeActivityRepository([_basketball, _riddles]);
    await _pump(tester, repository);

    await tester.tap(find.byIcon(Icons.settings));
    await tester.pumpAndSettle();
    expect(find.text('Ghicitori'), findsOneWidget);

    // Simulate an edit made on the web while the modal is open.
    repository.enabled = [_basketball];
    await tester.tap(find.text('Actualizează'));
    await tester.pumpAndSettle();

    expect(find.text('Baschet'), findsOneWidget);
    expect(find.text('Ghicitori'), findsNothing);
  });

  testWidgets('a failed refresh inside the modal keeps the cached '
      'activities visible', (tester) async {
    final repository = _FakeActivityRepository([_basketball, _riddles]);
    await _pump(tester, repository);

    await tester.tap(find.byIcon(Icons.settings));
    await tester.pumpAndSettle();

    repository.error = Exception('offline');
    await tester.tap(find.text('Actualizează'));
    await tester.pumpAndSettle();

    expect(find.text('Baschet'), findsOneWidget);
    expect(find.text('Ghicitori'), findsOneWidget);
  });

  testWidgets(
    'when there are no enabled activities, the settings modal says so',
    (tester) async {
      final repository = _FakeActivityRepository([]);
      await _pump(tester, repository);

      await tester.tap(find.byIcon(Icons.settings));
      await tester.pumpAndSettle();

      expect(find.text('Nicio activitate disponibilă'), findsOneWidget);
    },
  );

  testWidgets(
    'the start/stop button shows the play icon while the session is not '
    'running, and no debug/status text is visible in the normal interface',
    (tester) async {
      final repository = _FakeActivityRepository([_basketball, _riddles]);
      await _pump(tester, repository);

      expect(find.byIcon(Icons.play_arrow_rounded), findsOneWidget);
      expect(find.byIcon(Icons.stop_rounded), findsNothing);
      // No permanent debug/status panel until explicitly long-pressed open.
      expect(find.textContaining('Activitate:'), findsNothing);
      expect(find.textContaining('Stare:'), findsNothing);
    },
  );

  testWidgets(
    'renders without a layout overflow on a real landscape-phone aspect '
    'ratio — regression test for the original width-only face sizing bug',
    (tester) async {
      final repository = _FakeActivityRepository([_basketball, _riddles]);
      tester.view.physicalSize = const Size(2400, 1080);
      tester.view.devicePixelRatio = 3.0;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);

      await _pump(tester, repository);

      expect(tester.takeException(), isNull);

      await tester.tap(find.byIcon(Icons.settings));
      await tester.pumpAndSettle();
      expect(tester.takeException(), isNull);
    },
  );

  group('activity cache staying in sync with the repository', () {
    // Regression coverage for: an activity created on the web would show
    // up once after a refresh, then silently disappear again — see
    // SettingsModal.onActivitiesRefreshed's doc comment for the root
    // cause (RobotHomeScreen's own cache was never updated by the
    // modal's refresh, so reopening reseeded from a stale snapshot).
    // There was never a hardcoded-repository fallback involved — every
    // test here uses the same single injected repository throughout,
    // exactly like the real app does once Supabase initializes.

    testWidgets(
      'an activity that appears after a refresh is still visible after '
      'closing and reopening the settings modal',
      (tester) async {
        final repository = _FakeActivityRepository([_basketball, _riddles]);
        await _pump(tester, repository);

        await tester.tap(find.byIcon(Icons.settings));
        await tester.pumpAndSettle();

        // An activity gets created on the web while the modal is open.
        repository.enabled = [_basketball, _riddles, _testActivitate];
        await tester.tap(find.text('Actualizează'));
        await tester.pumpAndSettle();
        expect(find.text('Test Activitate'), findsOneWidget);

        await tester.tap(find.text('Închide'));
        await tester.pumpAndSettle();

        // Reopening — with no further repository change — must still show
        // all three; this is exactly where the bug used to lose the third.
        await tester.tap(find.byIcon(Icons.settings));
        await tester.pumpAndSettle();

        expect(find.text('Baschet'), findsOneWidget);
        expect(find.text('Ghicitori'), findsOneWidget);
        expect(find.text('Test Activitate'), findsOneWidget);
      },
    );

    testWidgets(
      'a second refresh preserves a remotely-created activity from the '
      'first refresh',
      (tester) async {
        final repository = _FakeActivityRepository([_basketball, _riddles]);
        await _pump(tester, repository);

        await tester.tap(find.byIcon(Icons.settings));
        await tester.pumpAndSettle();
        repository.enabled = [_basketball, _riddles, _testActivitate];
        await tester.tap(find.text('Actualizează'));
        await tester.pumpAndSettle();

        // Nothing changed remotely; refresh again.
        await tester.tap(find.text('Actualizează'));
        await tester.pumpAndSettle();

        expect(find.text('Test Activitate'), findsOneWidget);
      },
    );

    testWidgets(
      'switching to a different activity does not discard a previously '
      'refreshed activity list',
      (tester) async {
        final repository = _FakeActivityRepository([_basketball, _riddles]);
        await _pump(tester, repository);

        await tester.tap(find.byIcon(Icons.settings));
        await tester.pumpAndSettle();
        repository.enabled = [_basketball, _riddles, _testActivitate];
        await tester.tap(find.text('Actualizează'));
        await tester.pumpAndSettle();

        await tester.tap(find.text('Baschet'));
        await tester.pumpAndSettle();

        await tester.tap(find.byIcon(Icons.settings));
        await tester.pumpAndSettle();

        expect(find.text('Test Activitate'), findsOneWidget);
      },
    );

    testWidgets(
      'a failed refresh never reverts to a smaller cached list — the full '
      'previously-known list (including a remotely-created activity) '
      'stays visible',
      (tester) async {
        final repository = _FakeActivityRepository([
          _basketball,
          _riddles,
          _testActivitate,
        ]);
        await _pump(tester, repository);

        await tester.tap(find.byIcon(Icons.settings));
        await tester.pumpAndSettle();
        expect(find.text('Test Activitate'), findsOneWidget);

        repository.error = Exception('offline');
        await tester.tap(find.text('Actualizează'));
        await tester.pumpAndSettle();

        expect(find.text('Baschet'), findsOneWidget);
        expect(find.text('Ghicitori'), findsOneWidget);
        expect(find.text('Test Activitate'), findsOneWidget);
      },
    );

    testWidgets(
      'if the running activity is disabled/removed remotely, a refresh '
      'reselects the first enabled activity by sort_order',
      (tester) async {
        final repository = _FakeActivityRepository([_basketball, _riddles]);
        await _pump(tester, repository);
        await _openDebugOverlay(tester);
        expect(find.textContaining('Activitate: Ghicitori'), findsOneWidget);

        await tester.tap(find.byIcon(Icons.settings));
        await tester.pumpAndSettle();
        // Ghicitori (the running activity) was disabled/deleted remotely.
        repository.enabled = [_basketball];
        await tester.tap(find.text('Actualizează'));
        await tester.pumpAndSettle();

        expect(find.textContaining('Activitate: Baschet'), findsOneWidget);
      },
    );

    testWidgets('a fresh app start always loads activities from the injected '
        'repository — there is no hardcoded fallback once it succeeds', (
      tester,
    ) async {
      final repository = _FakeActivityRepository([
        _basketball,
        _riddles,
        _testActivitate,
      ]);
      await _pump(tester, repository);
      await _openDebugOverlay(tester);

      expect(repository.callCount, 1);
      // Ghicitori (sort_order 1) wins — proves the repository's actual
      // data drives the default, not a Baschet-first assumption.
      expect(find.textContaining('Activitate: Ghicitori'), findsOneWidget);

      await tester.tap(find.byIcon(Icons.settings));
      await tester.pumpAndSettle();
      expect(find.text('Test Activitate'), findsOneWidget);
    });
  });

  group('the currently-selected activity reconfigures when its own definition '
      'changes on refresh (not just when it disappears)', () {
    // All fixtures here stay push-to-talk-and-never-pressed or, when
    // testing a switch *to* free conversation, deliberately exercised
    // only while the global session is stopped — see the top-of-file
    // note on why: VoiceSessionController.startFreeConversation()/
    // startSession() dial a real Gemini Live connection, which a widget
    // test must never trigger.

    Future<void> selectBaschet(WidgetTester tester) async {
      await tester.tap(find.byIcon(Icons.settings));
      await tester.pumpAndSettle();
      await tester.tap(find.text('Baschet'));
      await tester.pumpAndSettle();
    }

    Future<void> refreshSettings(WidgetTester tester) async {
      await tester.tap(find.byIcon(Icons.settings));
      await tester.pumpAndSettle();
      await tester.tap(find.text('Actualizează'));
      await tester.pumpAndSettle();
      await tester.tap(find.text('Închide'));
      await tester.pumpAndSettle();
    }

    testWidgets(
      'a prompt edit on the same activity id replaces the stale composed '
      'system instruction the live controller was built from',
      (tester) async {
        final repository = _FakeActivityRepository([_basketball, _riddles]);
        await _pump(tester, repository);
        await selectBaschet(tester);
        await _openDebugOverlay(tester);

        expect(find.textContaining('NEW_PROMPT_MARKER'), findsNothing);

        // Edited on the web: same id, new prompt.
        repository.enabled = [
          _basketball.copyWith(systemPrompt: 'NEW_PROMPT_MARKER'),
          _riddles,
        ];
        await refreshSettings(tester);

        expect(find.textContaining('NEW_PROMPT_MARKER'), findsOneWidget);
      },
    );

    testWidgets(
      'a participants edit on the same activity id reaches the composed '
      'prompt the live controller uses',
      (tester) async {
        final repository = _FakeActivityRepository([_basketball, _riddles]);
        await _pump(tester, repository);
        await selectBaschet(tester);
        await _openDebugOverlay(tester);

        expect(find.textContaining('Robert'), findsNothing);

        repository.enabled = [
          _basketball.copyWith(participants: ['Robert', 'Elena']),
          _riddles,
        ];
        await refreshSettings(tester);

        expect(find.textContaining('Robert'), findsOneWidget);
      },
    );

    testWidgets(
      'interaction_mode push_to_talk -> free_conversation reconfigures '
      'the controller, and — since the global session is stopped — does '
      'not auto-start it',
      (tester) async {
        final repository = _FakeActivityRepository([_basketball, _riddles]);
        await _pump(tester, repository);
        await selectBaschet(tester);
        await _openDebugOverlay(tester);

        expect(find.textContaining('Mod: pushToTalk'), findsOneWidget);
        // The PTT touch target exists while push-to-talk is current.
        expect(find.byKey(const ValueKey('ptt-mouth-gesture')), findsOneWidget);

        repository.enabled = [
          _basketball.copyWith(
            interactionMode: InteractionMode.freeConversation,
          ),
          _riddles,
        ];
        await refreshSettings(tester);

        expect(find.textContaining('Mod: freeConversation'), findsOneWidget);
        // Never started — the session was stopped before the refresh, so
        // this must not auto-start free conversation on its own.
        expect(find.textContaining('Stare: disconnected'), findsOneWidget);
        // The gesture itself is gone, not just visually hidden — it's
        // conditionally built only for push-to-talk.
        expect(find.byKey(const ValueKey('ptt-mouth-gesture')), findsNothing);
      },
    );

    testWidgets(
      'interaction_mode free_conversation -> push_to_talk reconfigures '
      'the controller back and restores the PTT touch target',
      (tester) async {
        final repository = _FakeActivityRepository([_basketball, _riddles]);
        await _pump(tester, repository);
        await selectBaschet(tester);

        // First move it to free conversation (stopped session — safe,
        // no auto-start, per the test above).
        repository.enabled = [
          _basketball.copyWith(
            interactionMode: InteractionMode.freeConversation,
          ),
          _riddles,
        ];
        await refreshSettings(tester);
        expect(find.byKey(const ValueKey('ptt-mouth-gesture')), findsNothing);

        // Now move it back.
        repository.enabled = [
          _basketball.copyWith(interactionMode: InteractionMode.pushToTalk),
          _riddles,
        ];
        await refreshSettings(tester);

        await _openDebugOverlay(tester);
        expect(find.textContaining('Mod: pushToTalk'), findsOneWidget);
        expect(find.textContaining('Stare: disconnected'), findsOneWidget);
        expect(find.byKey(const ValueKey('ptt-mouth-gesture')), findsOneWidget);
      },
    );

    // Not exercised here: "refresh while the session is STARTED
    // preserves the started state across a mode change". Reaching a
    // genuinely started state (ConversationState past `disconnected`)
    // requires VoiceSessionController to actually connect, which means a
    // real Gemini Live WebSocket — RobotHomeScreen doesn't inject a fake
    // live service, and this suite deliberately never triggers real
    // network (see the top-of-file note). The decision logic itself
    // (_openSettings capturing `wasRunning` before suspending, and only
    // starting the reconfigured controller when it was true) was
    // verified by code review and by the physical-device regression
    // test in the task's own test plan (steps A-E), not by a widget
    // test.
  });
}
