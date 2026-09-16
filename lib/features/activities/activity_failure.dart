import 'package:supabase_flutter/supabase_flutter.dart';

/// Why an activity read or write failed, in the only terms the editor
/// shows: raw Supabase/PostgREST errors are never displayed or logged (they
/// can carry server text, request details or row content).
enum ActivityFailure {
  /// The database refused the operation (RLS / grants): signed in, but not
  /// the approved administrator — or not signed in at all.
  permissionDenied,

  /// The update/delete matched no row: either not authorised (RLS silently
  /// filters rows) or the activity no longer exists.
  notChanged,

  /// The session is no longer valid; the editor returns to the login page.
  sessionExpired,

  /// Anything else — network, server, unexpected response.
  unavailable,
}

enum ActivityAction { load, save, delete }

/// Thrown by [SupabaseActivityRepository] instead of the underlying
/// Supabase exception. Its [toString] names only the [failure] kind.
class ActivityRepositoryException implements Exception {
  const ActivityRepositoryException(this.failure);

  final ActivityFailure failure;

  @override
  String toString() => 'ActivityRepositoryException(${failure.name})';
}

/// The failure kind of any error thrown by an [ActivityRepository]. Errors
/// that are not an [ActivityRepositoryException] count as [unavailable].
ActivityFailure activityFailureOf(Object error) =>
    error is ActivityRepositoryException
    ? error.failure
    : ActivityFailure.unavailable;

/// Maps a Supabase client error to an [ActivityFailure].
/// [hasSession] tells a refused write by a signed-in user apart from one
/// sent after the session disappeared (the request then ran as `anon`).
ActivityFailure classifySupabaseError(
  Object error, {
  required bool hasSession,
}) {
  if (error is ActivityRepositoryException) return error.failure;
  if (error is AuthException) return ActivityFailure.sessionExpired;
  if (error is PostgrestException) {
    switch (error.code) {
      // insufficient_privilege: an RLS WITH CHECK failure or a revoked grant.
      case '42501':
      case '403':
        return hasSession
            ? ActivityFailure.permissionDenied
            : ActivityFailure.sessionExpired;
      // `.single()` found no row: RLS filtered it or it no longer exists.
      case 'PGRST116':
        return ActivityFailure.notChanged;
      // PostgREST JWT errors (expired / invalid / claims) and bare 401s.
      case 'PGRST301':
      case 'PGRST302':
      case 'PGRST303':
      case '401':
        return ActivityFailure.sessionExpired;
    }
  }
  return ActivityFailure.unavailable;
}

/// Fixed, user-facing Romanian message for a failure. Never includes error
/// text from the server.
String activityFailureMessage(ActivityFailure failure, ActivityAction action) {
  final prefix = switch (action) {
    ActivityAction.load => 'Nu am putut încărca activitățile',
    ActivityAction.save => 'Eroare la salvare',
    ActivityAction.delete => 'Eroare la ștergere',
  };
  final reason = switch (failure) {
    ActivityFailure.permissionDenied =>
      action == ActivityAction.load
          ? 'nu ai permisiunea să vezi activitățile.'
          : 'nu ai permisiunea să modifici activitățile.',
    ActivityFailure.notChanged =>
      'activitatea nu a fost modificată (nu ai permisiunea sau nu mai există).',
    ActivityFailure.sessionExpired =>
      'sesiunea a expirat. Autentifică-te din nou.',
    ActivityFailure.unavailable =>
      'nu am putut contacta serverul. Încearcă din nou.',
  };
  return '$prefix: $reason';
}
