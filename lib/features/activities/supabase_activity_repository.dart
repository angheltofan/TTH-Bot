import 'package:supabase_flutter/supabase_flutter.dart';

import 'activity.dart';
import 'activity_failure.dart';
import 'activity_repository.dart';

/// Normal runtime [ActivityRepository]: reads/writes the `activities`
/// table (see supabase/migrations). No caching — every call hits the
/// network, which is deliberate for this phase (see the "REFRESH
/// BEHAVIOR" scope: manual/lifecycle reloads only, no Realtime yet).
///
/// Writes are allowed by the database only for the allow-listed
/// administrator (supabase/migrations/*_activity_admin_writes.sql). Every
/// Supabase error is converted to an [ActivityRepositoryException], so no
/// raw server error reaches the UI or the logs.
class SupabaseActivityRepository implements ActivityRepository {
  SupabaseActivityRepository(this._client);

  final SupabaseClient _client;
  static const String _table = 'activities';

  @override
  Future<List<Activity>> getEnabledActivities() => _guard(() async {
    final rows = await _client
        .from(_table)
        .select()
        .eq('enabled', true)
        .order('sort_order')
        .order('title');
    return _toActivities(rows);
  });

  @override
  Future<List<Activity>> getAllActivities() => _guard(() async {
    final rows = await _client
        .from(_table)
        .select()
        .order('sort_order')
        .order('title');
    return _toActivities(rows);
  });

  @override
  Future<Activity> getActivity(String id) => _guard(() async {
    final row = await _client.from(_table).select().eq('id', id).single();
    return Activity.fromMap(row);
  });

  @override
  Future<Activity> createActivity(Activity activity) => _guard(() async {
    final row = await _client
        .from(_table)
        .insert(activity.toInsertMap())
        .select()
        .single();
    return Activity.fromMap(row);
  });

  @override
  Future<Activity> updateActivity(Activity activity) => _guard(() async {
    // `.single()` fails (PGRST116) when RLS filtered the row out, so an
    // unauthorised update can never look successful.
    final row = await _client
        .from(_table)
        .update(activity.toUpdateMap())
        .eq('id', activity.id)
        .select()
        .single();
    return Activity.fromMap(row);
  });

  @override
  Future<void> deleteActivity(String id) => _guard(() async {
    // A DELETE that RLS refuses affects 0 rows and returns no error, so ask
    // for the deleted rows back and require exactly one.
    final rows = await _client.from(_table).delete().eq('id', id).select('id');
    if (rows.length != 1) {
      throw const ActivityRepositoryException(ActivityFailure.notChanged);
    }
  });

  Future<T> _guard<T>(Future<T> Function() operation) async {
    try {
      return await operation();
    } on ActivityRepositoryException {
      rethrow;
    } catch (error) {
      throw ActivityRepositoryException(
        classifySupabaseError(
          error,
          hasSession: _client.auth.currentSession != null,
        ),
      );
    }
  }

  List<Activity> _toActivities(List<Map<String, dynamic>> rows) =>
      rows.map(Activity.fromMap).toList();
}
