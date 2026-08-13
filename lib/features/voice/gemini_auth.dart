import 'package:flutter/foundation.dart';

/// How [GeminiLiveService] authenticates its WebSocket connection to the
/// Gemini Live API.
///
/// Kept as a tiny, isolated abstraction so [GeminiLiveService]'s
/// WebSocket/audio logic never needs to know which mode is active — it
/// only reads [queryParameter] when building the connection URL.
@immutable
sealed class GeminiAuth {
  const GeminiAuth();

  /// The query parameter (name and value) [GeminiLiveService] appends to
  /// the WebSocket connection URL for this auth mode.
  String get queryParameter;
}

/// DEV-ONLY: a raw, long-lived Gemini API key, supplied via
/// `--dart-define=GEMINI_API_KEY=...`. Kept as a tested fallback for local
/// development — never the preferred production path, see
/// [GeminiEphemeralTokenAuth].
class GeminiApiKeyAuth extends GeminiAuth {
  const GeminiApiKeyAuth(this.apiKey);
  final String apiKey;

  @override
  String get queryParameter => 'key=$apiKey';
}

/// Preferred path: a short-lived token minted by the `gemini-token`
/// Supabase Edge Function (see `GeminiTokenService`) — the permanent
/// Gemini API key never leaves that Edge Function, let alone this app.
class GeminiEphemeralTokenAuth extends GeminiAuth {
  const GeminiEphemeralTokenAuth(this.token);
  final String token;

  @override
  String get queryParameter => 'access_token=$token';
}

/// Picks the best available auth mode for one voice session.
///
/// Tries [fetchEphemeralToken] first — normally `GeminiTokenService(
/// client).fetchEphemeralToken`, the preferred, production-safe path — and
/// only falls back to [GeminiApiKeyAuth] with [devApiKey] if that's
/// unavailable (no Edge Function deployed yet, no network, misconfigured
/// secret, ...). Takes a plain function rather than a `GeminiTokenService`
/// so this branching logic has no dependency on the Supabase SDK and can
/// be unit tested with a trivial fake.
///
/// Throws a [StateError] with a clear message if neither works, so
/// [VoiceSessionController] surfaces one readable error instead of
/// silently failing to authenticate.
Future<GeminiAuth> resolveGeminiAuth({
  required Future<String> Function()? fetchEphemeralToken,
  required String devApiKey,
}) async {
  if (fetchEphemeralToken != null) {
    try {
      final token = await fetchEphemeralToken();
      debugPrint('[GeminiAuth] using ephemeral token from gemini-token');
      return GeminiEphemeralTokenAuth(token);
    } catch (e) {
      debugPrint(
        '[GeminiAuth] ephemeral token unavailable ($e), falling back to '
        'dev API key mode',
      );
    }
  }

  if (devApiKey.isEmpty) {
    throw StateError(
      'Nicio metodă de autentificare Gemini disponibilă: funcția Edge '
      '"gemini-token" nu a răspuns și nu a fost furnizată o cheie API de '
      'dezvoltare (--dart-define=GEMINI_API_KEY=...).',
    );
  }
  debugPrint('[GeminiAuth] using dev API key mode');
  return GeminiApiKeyAuth(devApiKey);
}
