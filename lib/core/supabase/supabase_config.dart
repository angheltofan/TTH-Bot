/// Public Supabase project connection details for TTH Bot.
///
/// A Supabase publishable key (like the legacy anon key it replaces) is
/// explicitly designed to be embedded in client apps — see
/// https://supabase.com/docs/guides/api/api-keys. It authenticates as the
/// low-privilege `anon` Postgres role; access is controlled entirely by Row
/// Level Security policies (see supabase/migrations), not by keeping this
/// value secret. It is safe to commit, unlike a Gemini API key or a
/// Supabase secret/service-role key — neither of which belongs anywhere in
/// this app (see gemini_live_service.dart / gemini_token_service.dart for
/// how Gemini auth is kept separate).
class SupabaseConfig {
  const SupabaseConfig._();

  static const String url = 'https://gashjedpdvcrzwryjiva.supabase.co';
  static const String publishableKey =
      'sb_publishable_oejOu5h583SWoWzswwqrTA_PCeR_VKX';
}
