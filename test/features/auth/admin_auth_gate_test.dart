import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tth_bot/core/theme/tth_web_theme.dart';
import 'package:tth_bot/features/activities/activity.dart';
import 'package:tth_bot/features/activities/activity_failure.dart';
import 'package:tth_bot/features/activities/activity_repository.dart';
import 'package:tth_bot/features/activities/web/activity_web_app.dart';
import 'package:tth_bot/features/auth/admin_auth.dart';
import 'package:tth_bot/features/auth/admin_auth_gate.dart';
import 'package:tth_bot/features/auth/admin_login_screen.dart';

import 'fake_admin_auth.dart';

const _basketball = Activity(
  id: 'baschet',
  title: 'Baschet',
  type: ActivityType.lesson,
  systemPrompt: 'Prompt secret de test pentru baschet.',
  interactionMode: InteractionMode.pushToTalk,
  participants: ['Maria', 'Sofia'],
);

class _Repository implements ActivityRepository {
  int loadCalls = 0;
  Object? loadError;

  @override
  Future<List<Activity>> getEnabledActivities() async => const [_basketball];

  @override
  Future<List<Activity>> getAllActivities() async {
    loadCalls++;
    if (loadError != null) throw loadError!;
    return const [_basketball];
  }

  @override
  Future<Activity> getActivity(String id) async => _basketball;

  @override
  Future<Activity> createActivity(Activity activity) async => activity;

  @override
  Future<Activity> updateActivity(Activity activity) async => activity;

  @override
  Future<void> deleteActivity(String id) async {}
}

const _desktop = Size(1280, 800);
const _phone = Size(360, 740);

Future<void> _pumpGate(
  WidgetTester tester, {
  required FakeAdminAuth auth,
  required _Repository repository,
  Size size = _desktop,
  bool settle = true,
}) async {
  tester.view.physicalSize = size;
  tester.view.devicePixelRatio = 1.0;
  addTearDown(tester.view.reset);
  await tester.pumpWidget(
    MaterialApp(
      theme: buildTthWebTheme(),
      home: AdminAuthGate(
        auth: auth,
        editorBuilder: (context, session) => ActivityWebApp(
          repository: repository,
          onLogout: session.logout,
          onSessionExpired: session.sessionExpired,
        ),
      ),
    ),
  );
  // pumpAndSettle cannot finish while a spinner animates (it would run the
  // periodic session checks for minutes of fake time), so the "checking"
  // tests pump single frames instead.
  if (settle) {
    await tester.pumpAndSettle();
  } else {
    await tester.pump();
  }
}

/// Unmounts the gate so its session-check timer is cancelled.
Future<void> _finish(WidgetTester tester) async {
  await tester.pumpWidget(const SizedBox());
}

Finder get _usernameField => find.widgetWithText(TextField, 'Utilizator');
Finder get _passwordField => find.widgetWithText(TextField, 'Parolă');

String _passwordText(WidgetTester tester) =>
    tester.widget<TextField>(_passwordField).controller!.text;

Future<void> _login(
  WidgetTester tester, {
  String username = 'admin',
  String password = fakeAdminPassword,
}) async {
  await tester.enterText(_usernameField, username);
  await tester.enterText(_passwordField, password);
  await tester.tap(find.widgetWithText(FilledButton, 'Autentificare'));
  await tester.pumpAndSettle();
}

bool _editorVisible() => find.text('Activitate nouă').evaluate().isNotEmpty;

void main() {
  testWidgets('an unauthenticated visitor sees only the login page', (
    tester,
  ) async {
    final auth = FakeAdminAuth();
    final repository = _Repository();
    await _pumpGate(tester, auth: auth, repository: repository);

    expect(_usernameField, findsOneWidget);
    expect(_passwordField, findsOneWidget);
    expect(_editorVisible(), isFalse);
    expect(find.text('Baschet'), findsNothing);
    expect(repository.loadCalls, 0);
    // No sign-up, no password reset, no guest access.
    for (final text in [
      'Înregistrare',
      'Creează cont',
      'Ai uitat parola?',
      'Resetează parola',
      'Continuă ca vizitator',
    ]) {
      expect(find.textContaining(text), findsNothing);
    }
    expect(find.byType(TextButton), findsNothing);
    await _finish(tester);
  });

  testWidgets('admin with the right password reaches the editor', (
    tester,
  ) async {
    final auth = FakeAdminAuth();
    final repository = _Repository();
    await _pumpGate(tester, auth: auth, repository: repository);

    await _login(tester);

    expect(auth.attemptedUsernames, ['admin']);
    expect(_editorVisible(), isTrue);
    expect(find.text('Baschet'), findsOneWidget);
    expect(repository.loadCalls, 1);
    expect(_usernameField, findsNothing);
    await _finish(tester);
  });

  testWidgets('a wrong password shows the generic failure and clears the '
      'password field', (tester) async {
    final auth = FakeAdminAuth();
    await _pumpGate(tester, auth: auth, repository: _Repository());

    await _login(tester, password: 'wrong-test-password');

    expect(find.text(adminLoginFailedMessage), findsOneWidget);
    expect(_passwordText(tester), isEmpty);
    expect(_editorVisible(), isFalse);
    await _finish(tester);
  });

  testWidgets('a wrong username fails with exactly the same message', (
    tester,
  ) async {
    final auth = FakeAdminAuth();
    await _pumpGate(tester, auth: auth, repository: _Repository());

    await _login(tester, username: 'administrator');

    expect(find.text(adminLoginFailedMessage), findsOneWidget);
    expect(_passwordText(tester), isEmpty);
    expect(_editorVisible(), isFalse);
    await _finish(tester);
  });

  testWidgets('empty fields fail generically without calling auth', (
    tester,
  ) async {
    final auth = FakeAdminAuth();
    await _pumpGate(tester, auth: auth, repository: _Repository());

    await _login(tester, username: '', password: '');
    expect(find.text(adminLoginFailedMessage), findsOneWidget);

    await _login(tester, username: 'admin', password: '');
    expect(find.text(adminLoginFailedMessage), findsOneWidget);

    expect(auth.attemptedUsernames, isEmpty);
    await _finish(tester);
  });

  testWidgets('the form is disabled while signing in and the password is '
      'cleared even on success', (tester) async {
    final auth = FakeAdminAuth()..holdSignIn = Completer<void>();
    await _pumpGate(tester, auth: auth, repository: _Repository());

    await tester.enterText(_usernameField, 'admin');
    await tester.enterText(_passwordField, fakeAdminPassword);
    await tester.tap(find.widgetWithText(FilledButton, 'Autentificare'));
    await tester.pump();

    expect(_passwordText(tester), isEmpty);
    expect(find.byType(CircularProgressIndicator), findsOneWidget);
    // A second submit while busy does nothing.
    await tester.tap(find.byType(FilledButton));
    await tester.pump();
    expect(auth.attemptedUsernames, hasLength(1));

    auth.holdSignIn!.complete();
    await tester.pumpAndSettle();
    expect(_editorVisible(), isTrue);
    await _finish(tester);
  });

  testWidgets('an existing valid session is restored without logging in', (
    tester,
  ) async {
    final auth = FakeAdminAuth(status: AdminSessionStatus.active);
    final repository = _Repository();
    await _pumpGate(tester, auth: auth, repository: repository);

    expect(_editorVisible(), isTrue);
    expect(_usernameField, findsNothing);
    expect(auth.attemptedUsernames, isEmpty);
    await _finish(tester);
  });

  testWidgets('a stored expired session waits for the refresh, then opens '
      'the editor', (tester) async {
    final auth = FakeAdminAuth(status: AdminSessionStatus.expired);
    await _pumpGate(
      tester,
      auth: auth,
      repository: _Repository(),
      settle: false,
    );

    expect(_editorVisible(), isFalse);
    expect(_usernameField, findsNothing);
    expect(find.byType(CircularProgressIndicator), findsOneWidget);

    auth.status = AdminSessionStatus.active;
    auth.emit(AdminAuthChange.signedIn);
    await tester.pumpAndSettle();

    expect(_editorVisible(), isTrue);
    await _finish(tester);
  });

  testWidgets('a stored session whose refresh fails returns to the login '
      'page with the expiry notice', (tester) async {
    final auth = FakeAdminAuth(status: AdminSessionStatus.expired);
    await _pumpGate(
      tester,
      auth: auth,
      repository: _Repository(),
      settle: false,
    );

    auth.status = AdminSessionStatus.signedOut;
    auth.emit(AdminAuthChange.sessionExpired);
    await tester.pumpAndSettle();

    expect(_usernameField, findsOneWidget);
    expect(find.text(sessionExpiredNotice), findsOneWidget);
    await _finish(tester);
  });

  testWidgets('logout returns to the login page and ends the session', (
    tester,
  ) async {
    final auth = FakeAdminAuth(status: AdminSessionStatus.active);
    await _pumpGate(tester, auth: auth, repository: _Repository());

    await tester.tap(find.text('Deconectare'));
    await tester.pumpAndSettle();

    expect(auth.signOutCalls, 1);
    expect(_usernameField, findsOneWidget);
    expect(_editorVisible(), isFalse);
    expect(find.text(sessionExpiredNotice), findsNothing);
    await _finish(tester);
  });

  testWidgets('logging in again after logout loads the activities again', (
    tester,
  ) async {
    final auth = FakeAdminAuth(status: AdminSessionStatus.active);
    final repository = _Repository();
    await _pumpGate(tester, auth: auth, repository: repository);
    await tester.tap(find.text('Deconectare'));
    await tester.pumpAndSettle();

    await _login(tester);

    expect(_editorVisible(), isTrue);
    expect(repository.loadCalls, 2);
    await _finish(tester);
  });

  testWidgets('an expired-session event closes an open form and shows the '
      'login page', (tester) async {
    final auth = FakeAdminAuth(status: AdminSessionStatus.active);
    await _pumpGate(tester, auth: auth, repository: _Repository());

    await tester.ensureVisible(find.text('Editează'));
    await tester.tap(find.text('Editează'));
    await tester.pumpAndSettle();
    expect(find.text('Editează activitate'), findsOneWidget);

    auth.status = AdminSessionStatus.signedOut;
    auth.emit(AdminAuthChange.sessionExpired);
    await tester.pumpAndSettle();

    expect(find.text('Editează activitate'), findsNothing);
    expect(find.text('Prompt secret de test pentru baschet.'), findsNothing);
    expect(_usernameField, findsOneWidget);
    expect(find.text(sessionExpiredNotice), findsOneWidget);
    await _finish(tester);
  });

  testWidgets('a signed-out event from another tab returns to the login '
      'page without the expiry notice', (tester) async {
    final auth = FakeAdminAuth(status: AdminSessionStatus.active);
    await _pumpGate(tester, auth: auth, repository: _Repository());

    auth.status = AdminSessionStatus.signedOut;
    auth.emit(AdminAuthChange.signedOut);
    await tester.pumpAndSettle();

    expect(_usernameField, findsOneWidget);
    expect(find.text(sessionExpiredNotice), findsNothing);
    await _finish(tester);
  });

  testWidgets('a session that stays expired is detected by the periodic '
      'check', (tester) async {
    final auth = FakeAdminAuth(status: AdminSessionStatus.active);
    await _pumpGate(tester, auth: auth, repository: _Repository());

    auth.status = AdminSessionStatus.expired;
    await tester.pump(const Duration(seconds: 20));
    // One expired check is tolerated: the automatic refresh may be running.
    expect(_editorVisible(), isTrue);

    await tester.pump(const Duration(seconds: 20));
    await tester.pumpAndSettle();
    expect(_usernameField, findsOneWidget);
    expect(find.text(sessionExpiredNotice), findsOneWidget);
    expect(auth.signOutCalls, 1);
    await _finish(tester);
  });

  testWidgets('a refresh that succeeds between checks keeps the editor', (
    tester,
  ) async {
    final auth = FakeAdminAuth(status: AdminSessionStatus.active);
    await _pumpGate(tester, auth: auth, repository: _Repository());

    auth.status = AdminSessionStatus.expired;
    await tester.pump(const Duration(seconds: 20));
    auth.status = AdminSessionStatus.active;
    await tester.pump(const Duration(seconds: 20));
    await tester.pump(const Duration(seconds: 20));

    expect(_editorVisible(), isTrue);
    expect(auth.signOutCalls, 0);
    await _finish(tester);
  });

  testWidgets('a request rejected for an expired session returns to the '
      'login page', (tester) async {
    final auth = FakeAdminAuth(status: AdminSessionStatus.active);
    final repository = _Repository()
      ..loadError = const ActivityRepositoryException(
        ActivityFailure.sessionExpired,
      );
    await _pumpGate(tester, auth: auth, repository: repository);

    expect(_usernameField, findsOneWidget);
    expect(find.text(sessionExpiredNotice), findsOneWidget);
    expect(auth.signOutCalls, 1);
    await _finish(tester);
  });

  testWidgets('the expiry notice disappears once the user tries again', (
    tester,
  ) async {
    final auth = FakeAdminAuth(status: AdminSessionStatus.active);
    await _pumpGate(tester, auth: auth, repository: _Repository());
    auth.status = AdminSessionStatus.signedOut;
    auth.emit(AdminAuthChange.sessionExpired);
    await tester.pumpAndSettle();

    await _login(tester, password: 'wrong-test-password');

    expect(find.text(sessionExpiredNotice), findsNothing);
    expect(find.text(adminLoginFailedMessage), findsOneWidget);
    await _finish(tester);
  });

  testWidgets('a build without a configured admin account shows no login '
      'form', (tester) async {
    final auth = FakeAdminAuth(isConfigured: false);
    await _pumpGate(tester, auth: auth, repository: _Repository());

    expect(_usernameField, findsNothing);
    expect(
      find.text('Autentificarea nu este configurată pentru această versiune.'),
      findsOneWidget,
    );
    await _finish(tester);
  });

  testWidgets('nothing sensitive is written to the debug log', (tester) async {
    final logs = <String>[];
    final originalDebugPrint = debugPrint;
    debugPrint = (String? message, {int? wrapWidth}) => logs.add('$message');
    try {
      final auth = FakeAdminAuth();
      final repository = _Repository();
      await _pumpGate(tester, auth: auth, repository: repository);

      await _login(tester, password: 'wrong-test-password');
      await _login(tester);
      await tester.tap(find.text('Deconectare'));
      await tester.pumpAndSettle();
      await _login(tester);
      repository.loadError = const ActivityRepositoryException(
        ActivityFailure.sessionExpired,
      );
      await tester.tap(find.byTooltip('Reîncarcă'));
      await tester.pumpAndSettle();
      await _finish(tester);
    } finally {
      debugPrint = originalDebugPrint;
    }

    final joined = logs.join('\n');
    for (final secret in [
      fakeAdminPassword,
      'wrong-test-password',
      '@',
      'Prompt secret de test',
      'access_token',
      'refresh_token',
      'Bearer',
    ]) {
      expect(joined, isNot(contains(secret)));
    }
  });

  group('responsive layout', () {
    for (final entry in {'phone': _phone, 'desktop': _desktop}.entries) {
      testWidgets('login page fits a ${entry.key} screen', (tester) async {
        await _pumpGate(
          tester,
          auth: FakeAdminAuth(),
          repository: _Repository(),
          size: entry.value,
        );

        expect(tester.takeException(), isNull);
        for (final finder in [
          _usernameField,
          _passwordField,
          find.widgetWithText(FilledButton, 'Autentificare'),
        ]) {
          final rect = tester.getRect(finder);
          expect(rect.left, greaterThanOrEqualTo(0));
          expect(rect.right, lessThanOrEqualTo(entry.value.width));
          expect(rect.bottom, lessThanOrEqualTo(entry.value.height));
        }
        await _finish(tester);
      });

      testWidgets('editor with logout fits a ${entry.key} screen', (
        tester,
      ) async {
        await _pumpGate(
          tester,
          auth: FakeAdminAuth(status: AdminSessionStatus.active),
          repository: _Repository(),
          size: entry.value,
        );

        expect(tester.takeException(), isNull);
        final logout = find.byTooltip('Deconectare');
        expect(logout, findsOneWidget);
        final logoutRect = tester.getRect(logout);
        expect(logoutRect.right, lessThanOrEqualTo(entry.value.width));
        expect(
          find.text('Deconectare'),
          entry.key == 'phone' ? findsNothing : findsOneWidget,
        );
        final cardRect = tester.getRect(find.byType(Card).first);
        expect(cardRect.left, greaterThanOrEqualTo(0));
        expect(cardRect.right, lessThanOrEqualTo(entry.value.width));

        // The form screen fits too.
        await tester.ensureVisible(find.text('Editează'));
        await tester.tap(find.text('Editează'));
        await tester.pumpAndSettle();
        expect(tester.takeException(), isNull);
        expect(
          tester.getRect(find.widgetWithText(TextFormField, 'Titlu')).right,
          lessThanOrEqualTo(entry.value.width),
        );
        await _finish(tester);
      });
    }
  });
}
