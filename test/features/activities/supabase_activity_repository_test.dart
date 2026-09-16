import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
// http is a transitive dependency of supabase_flutter; only its test
// client is used, to run the real Supabase client without any network.
// ignore: depend_on_referenced_packages
import 'package:http/http.dart' as http;
// ignore: depend_on_referenced_packages
import 'package:http/testing.dart';
import 'package:supabase_flutter/supabase_flutter.dart';
import 'package:tth_bot/features/activities/activity.dart';
import 'package:tth_bot/features/activities/activity_failure.dart';
import 'package:tth_bot/features/activities/supabase_activity_repository.dart';

const _id = '00000000-0000-4000-8000-0000000000aa';

Map<String, dynamic> _row() => {
  'id': _id,
  'title': 'Baschet',
  'type': 'lesson',
  'prompt': 'x',
  'participants': <String>[],
  'interaction_mode': 'push_to_talk',
  'enabled': true,
  'sort_order': 0,
  'created_at': '2026-09-16T00:00:00Z',
  'updated_at': '2026-09-16T00:00:00Z',
};

http.Response _json(Object body, int status) => http.Response(
  jsonEncode(body),
  status,
  headers: {'content-type': 'application/json'},
);

http.Response _pgError(String code, int status) => _json({
  'code': code,
  'message': 'server text that must never be shown',
  'details': null,
  'hint': null,
}, status);

Future<({SupabaseActivityRepository repository, List<http.Request> requests})>
_repository(
  http.Response Function(http.Request request) respond, {
  bool signedIn = false,
}) async {
  final requests = <http.Request>[];
  final client = SupabaseClient(
    'https://example.invalid',
    'test-publishable-key',
    authOptions: const AuthClientOptions(autoRefreshToken: false),
    postgrestOptions: const PostgrestClientOptions(retryEnabled: false),
    httpClient: MockClient((request) async {
      requests.add(request);
      final response = respond(request);
      // postgrest reads the originating request from the response.
      return http.Response.bytes(
        response.bodyBytes,
        response.statusCode,
        headers: response.headers,
        request: request,
      );
    }),
  );
  addTearDown(client.dispose);
  if (signedIn) {
    await client.auth.setInitialSession(
      jsonEncode({
        'access_token': 'test-access-token-value',
        'token_type': 'bearer',
        'expires_in': 3600,
        'refresh_token': 'test-refresh-token-value',
        'user': {
          'id': '00000000-0000-4000-8000-000000000001',
          'aud': 'authenticated',
          'app_metadata': <String, dynamic>{},
          'user_metadata': <String, dynamic>{},
          'created_at': '2026-09-16T00:00:00Z',
        },
      }),
    );
  }
  return (repository: SupabaseActivityRepository(client), requests: requests);
}

Matcher _failsWith(ActivityFailure failure) => throwsA(
  isA<ActivityRepositoryException>().having(
    (e) => e.failure,
    'failure',
    failure,
  ),
);

const _activity = Activity(
  id: _id,
  title: 'Baschet',
  type: ActivityType.lesson,
  systemPrompt: 'x',
  interactionMode: InteractionMode.pushToTalk,
);

void main() {
  group('deleteActivity', () {
    test('asks for the deleted row back and accepts exactly one', () async {
      final r = await _repository(
        (_) => _json([
          {'id': _id},
        ], 200),
        signedIn: true,
      );

      await r.repository.deleteActivity(_id);

      final request = r.requests.single;
      expect(request.method, 'DELETE');
      expect(request.url.queryParameters['id'], 'eq.$_id');
      expect(request.url.queryParameters['select'], 'id');
      expect(request.headers['Prefer'], contains('return=representation'));
    });

    test('a delete that removed no row fails as notChanged (RLS refusal '
        'looks exactly like this)', () async {
      final r = await _repository((_) => _json([], 200), signedIn: true);
      await expectLater(
        r.repository.deleteActivity(_id),
        _failsWith(ActivityFailure.notChanged),
      );
    });

    test('a revoked grant fails as permissionDenied when signed in', () async {
      final r = await _repository(
        (_) => _pgError('42501', 403),
        signedIn: true,
      );
      await expectLater(
        r.repository.deleteActivity(_id),
        _failsWith(ActivityFailure.permissionDenied),
      );
    });

    test('a refusal without any session fails as sessionExpired', () async {
      final r = await _repository((_) => _pgError('42501', 401));
      await expectLater(
        r.repository.deleteActivity(_id),
        _failsWith(ActivityFailure.sessionExpired),
      );
    });
  });

  group('writes', () {
    test('an insert refused by RLS fails as permissionDenied', () async {
      final r = await _repository(
        (_) => _pgError('42501', 403),
        signedIn: true,
      );
      await expectLater(
        r.repository.createActivity(_activity),
        _failsWith(ActivityFailure.permissionDenied),
      );
    });

    test('an update that matched no row fails as notChanged', () async {
      final r = await _repository(
        (_) => _pgError('PGRST116', 406),
        signedIn: true,
      );
      await expectLater(
        r.repository.updateActivity(_activity),
        _failsWith(ActivityFailure.notChanged),
      );
    });

    test('an expired JWT fails as sessionExpired', () async {
      final r = await _repository(
        (_) => _pgError('PGRST303', 401),
        signedIn: true,
      );
      await expectLater(
        r.repository.updateActivity(_activity),
        _failsWith(ActivityFailure.sessionExpired),
      );
    });

    test('a successful update returns the stored row', () async {
      final r = await _repository((_) => _json(_row(), 200), signedIn: true);
      final updated = await r.repository.updateActivity(_activity);
      expect(updated.title, 'Baschet');
      expect(r.requests.single.method, 'PATCH');
    });
  });

  group('reads', () {
    test('anonymous reads still work', () async {
      final r = await _repository((_) => _json([_row()], 200));
      final activities = await r.repository.getAllActivities();
      expect(activities.single.id, _id);
    });

    test('network and server failures become unavailable', () async {
      final offline = await _repository(
        (_) => throw http.ClientException('offline'),
      );
      await expectLater(
        offline.repository.getAllActivities(),
        _failsWith(ActivityFailure.unavailable),
      );

      final broken = await _repository((_) => _pgError('XX000', 500));
      await expectLater(
        broken.repository.getAllActivities(),
        _failsWith(ActivityFailure.unavailable),
      );
    });
  });
}
