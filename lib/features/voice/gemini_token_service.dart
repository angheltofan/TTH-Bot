import 'package:supabase_flutter/supabase_flutter.dart';

/// Requests a short-lived Gemini Live ephemeral token from the
/// `gemini-token` Supabase Edge Function, so the permanent Gemini API key
/// never ships inside the app — see supabase/functions/gemini-token.
class GeminiTokenService {
  GeminiTokenService(this._client);

  final SupabaseClient _client;

  /// Returns one ephemeral token string, ready to wrap in
  /// [GeminiEphemeralTokenAuth]. Throws on any network/HTTP failure or an
  /// unexpected response shape — callers should treat any failure as
  /// "ephemeral tokens unavailable right now" and fall back rather than
  /// retry indefinitely (see `resolveGeminiAuth`).
  Future<String> fetchEphemeralToken() async {
    final response = await _client.functions.invoke('gemini-token');
    final data = response.data;
    if (data is Map && data['token'] is String) {
      return data['token'] as String;
    }
    throw FormatException('Unexpected gemini-token response shape: $data');
  }
}
