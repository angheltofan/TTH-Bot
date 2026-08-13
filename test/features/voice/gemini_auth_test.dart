import 'package:flutter_test/flutter_test.dart';
import 'package:tth_bot/features/voice/gemini_auth.dart';

void main() {
  group('GeminiApiKeyAuth / GeminiEphemeralTokenAuth', () {
    test('builds the expected query parameters', () {
      expect(const GeminiApiKeyAuth('my-key').queryParameter, 'key=my-key');
      expect(
        const GeminiEphemeralTokenAuth('eph-token').queryParameter,
        'access_token=eph-token',
      );
    });
  });

  group('resolveGeminiAuth', () {
    test('prefers a working ephemeral token over the dev API key', () async {
      final auth = await resolveGeminiAuth(
        fetchEphemeralToken: () async => 'eph-token',
        devApiKey: 'dev-key',
      );

      expect(auth, isA<GeminiEphemeralTokenAuth>());
      expect((auth as GeminiEphemeralTokenAuth).token, 'eph-token');
    });

    test('falls back to the dev API key if the token fetch throws', () async {
      final auth = await resolveGeminiAuth(
        fetchEphemeralToken: () async =>
            throw Exception('function not deployed'),
        devApiKey: 'dev-key',
      );

      expect(auth, isA<GeminiApiKeyAuth>());
      expect((auth as GeminiApiKeyAuth).apiKey, 'dev-key');
    });

    test(
      'uses the dev API key directly when no token fetcher is given',
      () async {
        final auth = await resolveGeminiAuth(
          fetchEphemeralToken: null,
          devApiKey: 'dev-key',
        );

        expect(auth, isA<GeminiApiKeyAuth>());
        expect((auth as GeminiApiKeyAuth).apiKey, 'dev-key');
      },
    );

    test(
      'throws a clear error when neither ephemeral token nor dev key work',
      () async {
        await expectLater(
          () => resolveGeminiAuth(
            fetchEphemeralToken: () async => throw Exception('offline'),
            devApiKey: '',
          ),
          throwsStateError,
        );
      },
    );

    test('throws when there is no token fetcher and no dev key', () async {
      await expectLater(
        () => resolveGeminiAuth(fetchEphemeralToken: null, devApiKey: ''),
        throwsStateError,
      );
    });
  });
}
