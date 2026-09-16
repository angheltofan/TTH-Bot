import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tth_bot/core/theme/tth_web_theme.dart';
import 'package:tth_bot/features/activities/activity.dart';
import 'package:tth_bot/features/activities/activity_failure.dart';
import 'package:tth_bot/features/activities/activity_repository.dart';
import 'package:tth_bot/features/activities/web/activity_web_app.dart';

const _basketball = Activity(
  id: 'baschet',
  title: 'Baschet',
  type: ActivityType.lesson,
  systemPrompt: 'x',
  interactionMode: InteractionMode.pushToTalk,
  participants: ['Maria', 'Sofia', 'Alex', 'Ștefan'],
);

const _riddles = Activity(
  id: 'ghicitori',
  title: 'Ghicitori',
  type: ActivityType.game,
  systemPrompt: 'y',
  interactionMode: InteractionMode.freeConversation,
  enabled: false,
);

class _FakeActivityRepository implements ActivityRepository {
  _FakeActivityRepository(this.activities);

  List<Activity> activities;
  Object? deleteError;
  Object? loadError;
  int deleteCallCount = 0;
  int loadCallCount = 0;

  @override
  Future<List<Activity>> getEnabledActivities() async =>
      activities.where((a) => a.enabled).toList();

  @override
  Future<List<Activity>> getAllActivities() async {
    loadCallCount++;
    if (loadError != null) throw loadError!;
    return activities;
  }

  @override
  Future<Activity> getActivity(String id) async =>
      activities.firstWhere((a) => a.id == id);

  @override
  Future<Activity> createActivity(Activity activity) async => activity;

  @override
  Future<Activity> updateActivity(Activity activity) async => activity;

  @override
  Future<void> deleteActivity(String id) async {
    deleteCallCount++;
    if (deleteError != null) throw deleteError!;
    activities = activities.where((a) => a.id != id).toList();
  }
}

Future<void> _pump(
  WidgetTester tester,
  ActivityRepository repository, {
  VoidCallback? onLogout,
  VoidCallback? onSessionExpired,
}) async {
  await tester.pumpWidget(
    MaterialApp(
      theme: buildTthWebTheme(),
      home: ActivityWebApp(
        repository: repository,
        onLogout: onLogout,
        onSessionExpired: onSessionExpired,
      ),
    ),
  );
  await tester.pumpAndSettle();
}

Future<void> _confirmDeleteFirst(WidgetTester tester) async {
  await tester.ensureVisible(find.text('Șterge').first);
  await tester.tap(find.text('Șterge').first);
  await tester.pumpAndSettle();
  await tester.tap(find.text('Șterge').last);
  await tester.pumpAndSettle();
}

void main() {
  testWidgets('renders the TTH Bot brand header and both activities', (
    tester,
  ) async {
    final repository = _FakeActivityRepository([_basketball, _riddles]);
    await _pump(tester, repository);

    expect(find.text('TTH Bot – Activități'), findsOneWidget);
    expect(find.text('Baschet'), findsOneWidget);
    expect(find.text('Ghicitori'), findsOneWidget);
  });

  testWidgets('participant order is preserved on the activity card', (
    tester,
  ) async {
    final repository = _FakeActivityRepository([_basketball]);
    await _pump(tester, repository);

    expect(find.text('Maria · Sofia · Alex · Ștefan'), findsOneWidget);
  });

  testWidgets('search filters the activity list by title', (tester) async {
    final repository = _FakeActivityRepository([_basketball, _riddles]);
    await _pump(tester, repository);

    await tester.enterText(
      find.widgetWithText(TextField, 'Caută o activitate...'),
      'basc',
    );
    await tester.pumpAndSettle();

    expect(find.text('Baschet'), findsOneWidget);
    expect(find.text('Ghicitori'), findsNothing);
  });

  testWidgets('deleting an activity asks for confirmation before removing '
      'it from the list', (tester) async {
    final repository = _FakeActivityRepository([_basketball, _riddles]);
    await _pump(tester, repository);

    await tester.ensureVisible(find.text('Șterge').first);
    await tester.tap(find.text('Șterge').first);
    await tester.pumpAndSettle();

    expect(find.text('Ștergi activitatea «Baschet»?'), findsOneWidget);

    // Cancelling must not delete.
    await tester.tap(find.text('Anulează'));
    await tester.pumpAndSettle();
    expect(repository.deleteCallCount, 0);
    expect(find.text('Baschet'), findsOneWidget);

    await tester.ensureVisible(find.text('Șterge').first);
    await tester.tap(find.text('Șterge').first);
    await tester.pumpAndSettle();
    await tester.tap(find.text('Șterge').last);
    await tester.pumpAndSettle();

    expect(repository.deleteCallCount, 1);
    expect(find.text('Baschet'), findsNothing);
    expect(find.text('Ghicitori'), findsOneWidget);
  });

  testWidgets('a failed delete keeps the activity visible and shows an '
      'error snackbar', (tester) async {
    final repository = _FakeActivityRepository([_basketball])
      ..deleteError = Exception('network down');
    await _pump(tester, repository);

    await tester.ensureVisible(find.text('Șterge').first);
    await tester.tap(find.text('Șterge').first);
    await tester.pumpAndSettle();
    await tester.tap(find.text('Șterge').last);
    await tester.pumpAndSettle();

    expect(find.text('Baschet'), findsOneWidget);
    expect(
      find.text(
        activityFailureMessage(
          ActivityFailure.unavailable,
          ActivityAction.delete,
        ),
      ),
      findsOneWidget,
    );
    // The raw error is never shown.
    expect(find.textContaining('network down'), findsNothing);
    expect(find.textContaining('Exception'), findsNothing);
  });

  testWidgets('a delete refused by the database shows the permission '
      'message', (tester) async {
    final repository = _FakeActivityRepository([_basketball])
      ..deleteError = const ActivityRepositoryException(
        ActivityFailure.permissionDenied,
      );
    await _pump(tester, repository);

    await _confirmDeleteFirst(tester);

    expect(
      find.text(
        'Eroare la ștergere: nu ai permisiunea să modifici activitățile.',
      ),
      findsOneWidget,
    );
    expect(find.text('Baschet'), findsOneWidget);
  });

  testWidgets('a delete that removed nothing says so and reloads the list', (
    tester,
  ) async {
    final repository = _FakeActivityRepository([_basketball])
      ..deleteError = const ActivityRepositoryException(
        ActivityFailure.notChanged,
      );
    await _pump(tester, repository);
    expect(repository.loadCallCount, 1);

    await _confirmDeleteFirst(tester);

    expect(
      find.text(
        activityFailureMessage(
          ActivityFailure.notChanged,
          ActivityAction.delete,
        ),
      ),
      findsOneWidget,
    );
    expect(repository.loadCallCount, 2);
  });

  testWidgets('a delete rejected for an expired session reports it to the '
      'gate instead of showing an error', (tester) async {
    var expired = 0;
    final repository = _FakeActivityRepository([_basketball])
      ..deleteError = const ActivityRepositoryException(
        ActivityFailure.sessionExpired,
      );
    await _pump(tester, repository, onSessionExpired: () => expired++);

    await _confirmDeleteFirst(tester);

    expect(expired, 1);
    expect(find.byType(SnackBar), findsNothing);
  });

  testWidgets('a failed load shows a fixed message, never the raw error', (
    tester,
  ) async {
    final repository = _FakeActivityRepository([_basketball])
      ..loadError = Exception('secret server detail');
    await _pump(tester, repository);

    expect(
      find.text(
        activityFailureMessage(
          ActivityFailure.unavailable,
          ActivityAction.load,
        ),
      ),
      findsOneWidget,
    );
    expect(find.textContaining('secret server detail'), findsNothing);
    expect(find.text('Încearcă din nou'), findsOneWidget);
  });

  testWidgets('a load rejected for an expired session reports it', (
    tester,
  ) async {
    var expired = 0;
    final repository = _FakeActivityRepository([_basketball])
      ..loadError = const ActivityRepositoryException(
        ActivityFailure.sessionExpired,
      );
    await _pump(tester, repository, onSessionExpired: () => expired++);

    expect(expired, 1);
  });

  testWidgets('the logout button appears only with a logout callback and '
      'calls it', (tester) async {
    final repository = _FakeActivityRepository([_basketball]);
    await _pump(tester, repository);
    expect(find.byTooltip('Deconectare'), findsNothing);

    var logouts = 0;
    await _pump(tester, repository, onLogout: () => logouts++);
    expect(find.byTooltip('Deconectare'), findsOneWidget);

    await tester.tap(find.byTooltip('Deconectare'));
    await tester.pump();
    expect(logouts, 1);
  });

  testWidgets('the refresh button is tappable above the scrollable page', (
    tester,
  ) async {
    final repository = _FakeActivityRepository([_basketball]);
    await _pump(tester, repository);
    expect(repository.loadCallCount, 1);

    await tester.tap(find.byTooltip('Reîncarcă'));
    await tester.pumpAndSettle();

    expect(repository.loadCallCount, 2);
  });

  testWidgets('the content column is centered with a bounded max width', (
    tester,
  ) async {
    final repository = _FakeActivityRepository([_basketball]);
    await _pump(tester, repository);

    // The page-level wrapper specifically (not the narrower search field's
    // own ConstrainedBox) — distinguished by width, since it's the widest
    // one on the page.
    final constrainedBox = tester.widget<ConstrainedBox>(
      find.byWidgetPredicate(
        (w) =>
            w is ConstrainedBox &&
            w.constraints.maxWidth > 900 &&
            w.constraints.maxWidth <= 1000,
      ),
    );
    expect(constrainedBox.constraints.maxWidth, lessThanOrEqualTo(1000));
  });
}
