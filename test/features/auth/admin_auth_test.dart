import 'dart:convert';

import 'package:flutter/foundation.dart';
import 'package:flutter_test/flutter_test.dart';
// http is a transitive dependency of supabase_flutter; only its test
// client is used, to run the real GoTrueClient without any network.
// ignore: depend_on_referenced_packages
import 'package:http/http.dart' as http;
// ignore: depend_on_referenced_packages
import 'package:http/testing.dart';
import 'package:supabase_flutter/supabase_flutter.dart';
import 'package:tth_bot/features/auth/admin_auth.dart';

import 'fake_admin_auth.dart';

// Test-only values. The real internal email is supplied at build time and
// the real password is typed by the administrator; neither is used here.
const _testAdminEmail = 'admin@example.invalid';
const _testAccessToken = 'test-access-token-value';
const _testRefreshToken = 'test-refresh-token-value';

Map<String, dynamic> _sessionJson() => {
  'access_token': _testAccessToken,
  'token_type': 'bearer',
  'expires_in': 3600,
  'refresh_token': _testRefreshToken,
  'user': {
    'id': '00000000-0000-4000-8000-000000000001',
    'aud': 'authenticated',
    'role': 'authenticated',
    'email': _testAdminEmail,
    'app_metadata': <String, dynamic>{},
    'user_metadata': <String, dynamic>{},
    'created_at': '2026-09-16T00:00:00Z',
  },
};

class _Harness {
  _Harness(this.respond) {
    client = GoTrueClient(
      url: 'https://example.invalid/auth/v1',
      autoRefreshToken: false,
      flowType: AuthFlowType.implicit,
      httpClient: MockClient((request) async {
        requests.add(request);
        return respond(request);
      }),
    );
    auth = SupabaseAdminAuth(client, adminEmail: _testAdminEmail);
  }

  final Future<http.Response> Function(http.Request request) respond;
  final List<http.Request> requests = [];
  late final GoTrueClient client;
  late final SupabaseAdminAuth auth;
}

http.Response _json(Object body, int status) => http.Response(
  jsonEncode(body),
  status,
  headers: {'content-type': 'application/json'},
);

void main() {
  late List<String> logs;
  late DebugPrintCallback originalDebugPrint;

  setUp(() {
    logs = [];
    originalDebugPrint = debugPrint;
    debugPrint = (String? message, {int? wrapWidth}) => logs.add('$message');
  });

  tearDown(() {
    debugPrint = originalDebugPrint;
    final joined = logs.join('\n');
    for (final secret in [
      fakeAdminPassword,
      _testAdminEmail,
      _testAccessToken,
      _testRefreshToken,
    ]) {
      expect(joined, isNot(contains(secret)));
    }
  });

  group('adminEmailForUsername', () {
    test('maps only "admin" to the configured email', () {
      expect(
        adminEmailForUsername('admin', adminEmail: _testAdminEmail),
        _testAdminEmail,
      );
      expect(
        adminEmailForUsername('  admin ', adminEmail: _testAdminEmail),
        _testAdminEmail,
      );
      for (final other in [
        '',
        'Admin',
        'ADMIN',
        'administrator',
        'admin2',
        _testAdminEmail,
      ]) {
        expect(
          adminEmailForUsername(other, adminEmail: _testAdminEmail),
          isNull,
        );
      }
    });

    test('fails closed when no valid admin email is configured', () {
      expect(adminEmailForUsername('admin', adminEmail: ''), isNull);
      expect(
        adminEmailForUsername('admin', adminEmail: 'not-an-email'),
        isNull,
      );
    });
  });

  group('SupabaseAdminAuth', () {
    test('is not configured without an admin email', () {
      final client = GoTrueClient(
        url: 'https://example.invalid/auth/v1',
        autoRefreshToken: false,
        httpClient: MockClient((_) async => http.Response('', 500)),
      );
      expect(SupabaseAdminAuth(client, adminEmail: '').isConfigured, isFalse);
      expect(
        SupabaseAdminAuth(client, adminEmail: _testAdminEmail).isConfigured,
        isTrue,
      );
    });

    test('a wrong username never reaches the network', () async {
      final h = _Harness((_) async => _json(_sessionJson(), 200));

      expect(
        await h.auth.signIn(username: 'root', password: fakeAdminPassword),
        isFalse,
      );
      expect(await h.auth.signIn(username: 'admin', password: ''), isFalse);
      expect(h.requests, isEmpty);
      expect(h.auth.sessionStatus, AdminSessionStatus.signedOut);
    });

    test(
      'admin signs in with the mapped email and the typed password',
      () async {
        final h = _Harness((_) async => _json(_sessionJson(), 200));
        final changes = <AdminAuthChange>[];
        final sub = h.auth.changes.listen(changes.add);

        final ok = await h.auth.signIn(
          username: 'admin',
          password: fakeAdminPassword,
        );
        await pumpEventQueue();

        expect(ok, isTrue);
        expect(h.requests, hasLength(1));
        final request = h.requests.single;
        expect(request.url.path, '/auth/v1/token');
        expect(request.url.queryParameters['grant_type'], 'password');
        final body = jsonDecode(request.body) as Map<String, dynamic>;
        expect(body['email'], _testAdminEmail);
        expect(body['password'], fakeAdminPassword);
        expect(h.auth.sessionStatus, AdminSessionStatus.active);
        expect(changes, contains(AdminAuthChange.signedIn));
        await sub.cancel();
      },
    );

    test('rejected credentials and server errors are a plain false', () async {
      for (final response in [
        _json({
          'code': 400,
          'error_code': 'invalid_credentials',
          'msg': 'Invalid login credentials',
        }, 400),
        _json({'message': 'boom'}, 500),
      ]) {
        final h = _Harness((_) async => response);
        expect(
          await h.auth.signIn(username: 'admin', password: fakeAdminPassword),
          isFalse,
        );
        expect(h.auth.sessionStatus, AdminSessionStatus.signedOut);
      }
    });

    test('a network failure is a plain false', () async {
      final h = _Harness((_) async => throw http.ClientException('offline'));
      expect(
        await h.auth.signIn(username: 'admin', password: fakeAdminPassword),
        isFalse,
      );
    });

    test(
      'signOut ends the session and reports a deliberate sign-out',
      () async {
        final h = _Harness((request) async {
          if (request.url.path.endsWith('/logout')) {
            return http.Response('', 204);
          }
          return _json(_sessionJson(), 200);
        });
        await h.auth.signIn(username: 'admin', password: fakeAdminPassword);
        final changes = <AdminAuthChange>[];
        final sub = h.auth.changes.listen(changes.add);
        await pumpEventQueue();
        changes.clear();

        await h.auth.signOut();
        await pumpEventQueue();

        expect(h.auth.sessionStatus, AdminSessionStatus.signedOut);
        expect(changes, [AdminAuthChange.signedOut]);
        await sub.cancel();
      },
    );

    test('signOut never throws, even when the server call fails', () async {
      final h = _Harness((request) async {
        if (request.url.path.endsWith('/logout')) {
          throw http.ClientException('offline');
        }
        return _json(_sessionJson(), 200);
      });
      await h.auth.signIn(username: 'admin', password: fakeAdminPassword);

      await h.auth.signOut();

      expect(h.auth.sessionStatus, AdminSessionStatus.signedOut);
    });

    test('a restored valid session is reported as signed in', () async {
      final h = _Harness((_) async => http.Response('', 500));
      final changes = <AdminAuthChange>[];
      final sub = h.auth.changes.listen(changes.add);

      await h.client.setInitialSession(jsonEncode(_sessionJson()));
      await pumpEventQueue();

      expect(h.auth.sessionStatus, AdminSessionStatus.active);
      expect(changes, contains(AdminAuthChange.signedIn));
      await sub.cancel();
    });

    test('an unusable stored session is reported as expired, without '
        'forwarding the error', () async {
      final h = _Harness((_) async => http.Response('', 500));
      final changes = <AdminAuthChange>[];
      final errors = <Object>[];
      final sub = h.auth.changes.listen(changes.add, onError: errors.add);

      await expectLater(
        h.client.setInitialSession(jsonEncode({'user': null})),
        throwsA(isA<AuthException>()),
      );
      await pumpEventQueue();

      expect(h.auth.sessionStatus, AdminSessionStatus.signedOut);
      expect(changes, contains(AdminAuthChange.sessionExpired));
      expect(errors, isEmpty);
      await sub.cancel();
    });
  });
}
