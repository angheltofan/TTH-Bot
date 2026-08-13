import 'package:supabase_flutter/supabase_flutter.dart';

import 'activity.dart';
import 'activity_repository.dart';

/// Normal runtime [ActivityRepository]: reads/writes the `activities`
/// table (see supabase/migrations). No caching — every call hits the
/// network, which is deliberate for this phase (see the "REFRESH
/// BEHAVIOR" scope: manual/lifecycle reloads only, no Realtime yet).
class SupabaseActivityRepository implements ActivityRepository {
  SupabaseActivityRepository(this._client);

  final SupabaseClient _client;
  static const String _table = 'activities';

  @override
  Future<List<Activity>> getEnabledActivities() async {
    final rows = await _client
        .from(_table)
        .select()
        .eq('enabled', true)
        .order('sort_order')
        .order('title');
    return _toActivities(rows);
  }

  @override
  Future<List<Activity>> getAllActivities() async {
    final rows = await _client
        .from(_table)
        .select()
        .order('sort_order')
        .order('title');
    return _toActivities(rows);
  }

  @override
  Future<Activity> getActivity(String id) async {
    final row = await _client.from(_table).select().eq('id', id).single();
    return Activity.fromMap(row);
  }

  @override
  Future<Activity> createActivity(Activity activity) async {
    final row = await _client
        .from(_table)
        .insert(activity.toInsertMap())
        .select()
        .single();
    return Activity.fromMap(row);
  }

  @override
  Future<Activity> updateActivity(Activity activity) async {
    final row = await _client
        .from(_table)
        .update(activity.toUpdateMap())
        .eq('id', activity.id)
        .select()
        .single();
    return Activity.fromMap(row);
  }

  @override
  Future<void> deleteActivity(String id) async {
    await _client.from(_table).delete().eq('id', id);
  }

  List<Activity> _toActivities(List<Map<String, dynamic>> rows) =>
      rows.map(Activity.fromMap).toList();
}
