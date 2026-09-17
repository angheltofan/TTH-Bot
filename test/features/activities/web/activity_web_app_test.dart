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
  sortOrder: 1,
);

const _freeTalk = Activity(
  id: 'conversatie',
  title: 'Conversație liberă',
  type: ActivityType.conversation,
  systemPrompt: 'z',
  interactionMode: InteractionMode.freeConversation,
  sortOrder: 2,
);

const _animals = Activity(
  id: 'animale',
  title: 'Animale',
  type: ActivityType.game,
  systemPrompt: 'w',
  interactionMode: InteractionMode.pushToTalk,
  sortOrder: 3,
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

const _desktop = Size(1280, 900);
const _medium = Size(820, 900);
const _phone = Size(390, 844);

Future<void> _pump(
  WidgetTester tester,
  ActivityRepository repository, {
  VoidCallback? onLogout,
  VoidCallback? onSessionExpired,
  Size size = _desktop,
}) async {
  tester.view.physicalSize = size;
  tester.view.devicePixelRatio = 1.0;
  addTearDown(tester.view.reset);
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

Future<void> _search(WidgetTester tester, String text) async {
  await tester.enterText(
    find.widgetWithText(TextField, 'Caută o activitate...'),
    text,
  );
  await tester.pumpAndSettle();
}

Future<void> _selectFilter(WidgetTester tester, String label) async {
  await tester.tap(find.byIcon(Icons.filter_list));
  await tester.pumpAndSettle();
  await tester.tap(find.text(label).last);
  await tester.pumpAndSettle();
}

/// Number of grid columns, from the x positions of the cards' titles.
int _columnCount(WidgetTester tester, List<String> titles) {
  final xs = <double>{};
  var topRowY = double.infinity;
  for (final title in titles) {
    final finder = find.text(title);
    if (finder.evaluate().isEmpty) continue;
    final rect = tester.getRect(finder);
    topRowY = rect.top < topRowY ? rect.top : topRowY;
  }
  for (final title in titles) {
    final finder = find.text(title);
    if (finder.evaluate().isEmpty) continue;
    final rect = tester.getRect(finder);
    if ((rect.top - topRowY).abs() < 1) xs.add(rect.left);
  }
  return xs.length;
}

void main() {
  group('dashboard layout', () {
    testWidgets('renders the brand header, title and both activities', (
      tester,
    ) async {
      final repository = _FakeActivityRepository([_basketball, _riddles]);
      await _pump(tester, repository);

      expect(find.text('Administrare TTH Bot'), findsOneWidget);
      expect(find.text('Activități TTH Bot'), findsOneWidget);
      expect(
        find.text('Creează și configurează activitățile robotului.'),
        findsOneWidget,
      );
      expect(find.text('Baschet'), findsOneWidget);
      expect(find.text('Ghicitori'), findsOneWidget);
    });

    testWidgets('the activity count uses Romanian singular and plural', (
      tester,
    ) async {
      expect(activityCountLabel(0), '0 activități');
      expect(activityCountLabel(1), '1 activitate');
      expect(activityCountLabel(4), '4 activități');

      await _pump(tester, _FakeActivityRepository([_basketball]));
      expect(find.text('1 activitate'), findsOneWidget);
      expect(find.text('1 activități'), findsNothing);
    });

    testWidgets('four activities are counted in the plural', (tester) async {
      await _pump(
        tester,
        _FakeActivityRepository([_basketball, _riddles, _freeTalk, _animals]),
      );

      expect(find.text('4 activități'), findsOneWidget);
    });

    testWidgets('the count follows creation and deletion', (tester) async {
      final repository = _FakeActivityRepository([_basketball, _riddles]);
      await _pump(tester, repository);
      expect(find.text('2 activități'), findsOneWidget);

      await _confirmDeleteFirst(tester);

      expect(find.text('1 activitate'), findsOneWidget);
    });

    testWidgets('three columns on a wide desktop', (tester) async {
      final repository = _FakeActivityRepository([
        _basketball,
        _riddles,
        _freeTalk,
        _animals,
      ]);
      await _pump(tester, repository, size: const Size(1440, 1000));

      expect(
        _columnCount(tester, [
          'Baschet',
          'Ghicitori',
          'Conversație liberă',
          'Animale',
        ]),
        3,
      );
    });

    testWidgets('two columns at medium width', (tester) async {
      final repository = _FakeActivityRepository([
        _basketball,
        _riddles,
        _freeTalk,
        _animals,
      ]);
      await _pump(tester, repository, size: _medium);

      expect(
        _columnCount(tester, [
          'Baschet',
          'Ghicitori',
          'Conversație liberă',
          'Animale',
        ]),
        2,
      );
    });

    testWidgets('one column on a phone, with no overflow', (tester) async {
      final repository = _FakeActivityRepository([
        _basketball,
        _riddles,
        _freeTalk,
        _animals,
      ]);
      await _pump(tester, repository, size: _phone);

      expect(tester.takeException(), isNull);
      expect(
        _columnCount(tester, [
          'Baschet',
          'Ghicitori',
          'Conversație liberă',
          'Animale',
        ]),
        1,
      );
      for (final finder in [
        find.text('Administrare TTH Bot'),
        find.text('Activități TTH Bot'),
        find.widgetWithText(TextField, 'Caută o activitate...'),
        find.widgetWithText(FilledButton, 'Activitate nouă'),
        find.text('Baschet'),
      ]) {
        final rect = tester.getRect(finder);
        expect(rect.left, greaterThanOrEqualTo(0));
        expect(rect.right, lessThanOrEqualTo(_phone.width));
      }
      // Phone header keeps icon-only actions with accessible labels.
      expect(find.byTooltip('Actualizează'), findsOneWidget);
      expect(find.text('Actualizează'), findsNothing);
    });

    testWidgets('cards in a row have the same height and the action buttons '
        'stay touch-sized', (tester) async {
      final repository = _FakeActivityRepository([
        _basketball,
        _riddles,
        _freeTalk,
      ]);
      await _pump(tester, repository);

      final heights = tester
          .widgetList<Card>(find.byType(Card))
          .map((_) => 0)
          .toList();
      expect(heights.length, 3);
      final rects = find
          .byType(Card)
          .evaluate()
          .map((e) => tester.getRect(find.byWidget(e.widget)))
          .toList();
      for (final rect in rects) {
        expect(rect.height, rects.first.height);
      }
      for (final button in tester.widgetList<OutlinedButton>(
        find.byType(OutlinedButton),
      )) {
        expect(button.style?.minimumSize?.resolve({})?.height, 44);
      }
    });

    testWidgets('the page content is centered with a bounded max width', (
      tester,
    ) async {
      await _pump(tester, _FakeActivityRepository([_basketball]));

      final constrainedBox = tester.widget<ConstrainedBox>(
        find.byWidgetPredicate(
          (w) =>
              w is ConstrainedBox &&
              w.constraints.maxWidth >= 1100 &&
              w.constraints.maxWidth <= 1160,
        ),
      );
      expect(constrainedBox.constraints.maxWidth, lessThanOrEqualTo(1160));
    });
  });

  group('activity card', () {
    testWidgets('shows the status pill and the activity type, but no '
        'interaction-mode chip or prompt text', (tester) async {
      final repository = _FakeActivityRepository([
        _basketball,
        _riddles,
        _freeTalk,
      ]);
      await _pump(tester, repository);

      expect(find.text('ACTIVĂ'), findsNWidgets(2));
      expect(find.text('INACTIVĂ'), findsOneWidget);
      expect(find.text('Lecție'), findsOneWidget);
      expect(find.text('Joc'), findsOneWidget);
      // The title stays exactly as stored, even when it names a mode.
      expect(find.text('Conversație liberă'), findsOneWidget);
      // ...but the interaction mode itself is never shown as a chip.
      expect(find.text('Push-to-talk'), findsNothing);
      expect(find.text('x'), findsNothing);
      expect(find.text('y'), findsNothing);
    });

    testWidgets('participants are summarised as a count, not listed', (
      tester,
    ) async {
      await _pump(tester, _FakeActivityRepository([_basketball, _freeTalk]));

      expect(find.text('4 participanți'), findsOneWidget);
      expect(find.text('Maria · Sofia · Alex · Ștefan'), findsNothing);
      expect(find.textContaining('Maria'), findsNothing);
    });
  });

  group('search and filtering', () {
    testWidgets('search filters the activity list by title', (tester) async {
      await _pump(tester, _FakeActivityRepository([_basketball, _riddles]));

      await _search(tester, 'basc');

      expect(find.text('Baschet'), findsOneWidget);
      expect(find.text('Ghicitori'), findsNothing);
    });

    testWidgets('the status filter shows only active or only inactive', (
      tester,
    ) async {
      final repository = _FakeActivityRepository([
        _basketball,
        _riddles,
        _freeTalk,
      ]);
      await _pump(tester, repository);

      await _selectFilter(tester, 'Active');
      expect(find.text('Baschet'), findsOneWidget);
      expect(find.text('Conversație liberă'), findsOneWidget);
      expect(find.text('Ghicitori'), findsNothing);

      await _selectFilter(tester, 'Inactive');
      expect(find.text('Ghicitori'), findsOneWidget);
      expect(find.text('Baschet'), findsNothing);

      await _selectFilter(tester, 'Toate activitățile');
      expect(find.text('Baschet'), findsOneWidget);
      expect(find.text('Ghicitori'), findsOneWidget);
    });

    testWidgets('search and status filter apply together', (tester) async {
      final repository = _FakeActivityRepository([
        _basketball,
        _riddles,
        _freeTalk,
        _animals,
      ]);
      await _pump(tester, repository);

      await _selectFilter(tester, 'Active');
      await _search(tester, 'a');

      // Active and containing "a": Baschet, Conversație liberă, Animale.
      expect(find.text('Baschet'), findsOneWidget);
      expect(find.text('Conversație liberă'), findsOneWidget);
      expect(find.text('Animale'), findsOneWidget);
      // Ghicitori contains no "a" and is inactive.
      expect(find.text('Ghicitori'), findsNothing);

      await _search(tester, 'ghici');
      expect(
        find.text('Nicio activitate nu corespunde căutării.'),
        findsOneWidget,
      );
    });

    testWidgets('filtering never re-queries the repository', (tester) async {
      final repository = _FakeActivityRepository([_basketball, _riddles]);
      await _pump(tester, repository);
      expect(repository.loadCallCount, 1);

      await _selectFilter(tester, 'Inactive');
      await _search(tester, 'ghi');

      expect(repository.loadCallCount, 1);
    });
  });

  group('states', () {
    testWidgets('an empty activity list says so', (tester) async {
      await _pump(tester, _FakeActivityRepository([]));

      expect(find.text('Nu există încă nicio activitate.'), findsOneWidget);
      expect(find.text('0 activități'), findsOneWidget);
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
  });

  group('header actions', () {
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

    testWidgets('refresh reloads and no widget covers the header buttons', (
      tester,
    ) async {
      final repository = _FakeActivityRepository([_basketball]);
      var logouts = 0;
      await _pump(tester, repository, onLogout: () => logouts++);
      expect(repository.loadCallCount, 1);

      // warnIfMissed would fire if another widget sat on top of them.
      await tester.tap(find.byTooltip('Actualizează'));
      await tester.pumpAndSettle();
      expect(repository.loadCallCount, 2);

      await tester.tap(find.byTooltip('Deconectare'));
      await tester.pump();
      expect(logouts, 1);
    });

    testWidgets('both header actions are reachable by keyboard', (
      tester,
    ) async {
      await _pump(
        tester,
        _FakeActivityRepository([_basketball]),
        onLogout: () {},
      );

      for (final tooltip in ['Actualizează', 'Deconectare']) {
        final focusNodes = tester
            .widgetList<Focus>(
              find.descendant(
                of: find.byTooltip(tooltip),
                matching: find.byType(Focus),
              ),
            )
            .toList();
        expect(focusNodes, isNotEmpty);
        expect(focusNodes.any((f) => f.canRequestFocus), isTrue);
      }
    });
  });

  group('create, edit and delete', () {
    testWidgets('the create button opens the form screen', (tester) async {
      await _pump(tester, _FakeActivityRepository([_basketball]));

      await tester.tap(find.widgetWithText(FilledButton, 'Activitate nouă'));
      await tester.pumpAndSettle();

      expect(find.text('Activitate nouă'), findsWidgets);
      expect(find.widgetWithText(TextFormField, 'Titlu'), findsOneWidget);
    });

    testWidgets('the edit button opens the form for that activity', (
      tester,
    ) async {
      await _pump(tester, _FakeActivityRepository([_basketball]));

      await tester.tap(find.text('Editează'));
      await tester.pumpAndSettle();

      expect(find.text('Editează activitate'), findsOneWidget);
      expect(find.widgetWithText(TextFormField, 'Baschet'), findsOneWidget);
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

      await _confirmDeleteFirst(tester);

      expect(repository.deleteCallCount, 1);
      expect(find.text('Baschet'), findsNothing);
      expect(find.text('Ghicitori'), findsOneWidget);
    });

    testWidgets('a failed delete keeps the activity visible and shows an '
        'error snackbar', (tester) async {
      final repository = _FakeActivityRepository([_basketball])
        ..deleteError = Exception('network down');
      await _pump(tester, repository);

      await _confirmDeleteFirst(tester);

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
  });
}
