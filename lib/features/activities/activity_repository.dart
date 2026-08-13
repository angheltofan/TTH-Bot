import 'activity.dart';
import 'hardcoded_activities.dart';

/// Where the app gets its activities from, and (for the web configuration
/// UI) how it edits them. [SupabaseActivityRepository] is the normal
/// runtime implementation; [HardcodedActivityRepository] stays as a
/// read-only fallback/testing implementation.
abstract class ActivityRepository {
  /// Activities the Android robot picker should offer: enabled only,
  /// ordered by [Activity.sortOrder] then title.
  Future<List<Activity>> getEnabledActivities();

  /// Every activity — enabled or not — for the web configuration list.
  Future<List<Activity>> getAllActivities();

  Future<Activity> getActivity(String id);

  /// Creates a new activity. [activity.id] is ignored — the store assigns
  /// the real id (a fresh UUID in Supabase).
  Future<Activity> createActivity(Activity activity);

  Future<Activity> updateActivity(Activity activity);

  Future<void> deleteActivity(String id);
}

class HardcodedActivityRepository implements ActivityRepository {
  const HardcodedActivityRepository();

  static const List<Activity> _activities = [
    basketballActivity,
    freeConversationActivity,
  ];

  @override
  Future<List<Activity>> getEnabledActivities() async {
    final activities = _activities.where((a) => a.enabled).toList()
      ..sort(_bySortOrderThenTitle);
    return activities;
  }

  @override
  Future<List<Activity>> getAllActivities() async {
    final activities = List<Activity>.of(_activities)
      ..sort(_bySortOrderThenTitle);
    return activities;
  }

  @override
  Future<Activity> getActivity(String id) async {
    return _activities.firstWhere(
      (a) => a.id == id,
      orElse: () => throw StateError('Activity not found: $id'),
    );
  }

  @override
  Future<Activity> createActivity(Activity activity) {
    throw UnsupportedError(
      'HardcodedActivityRepository is read-only; use SupabaseActivityRepository '
      'to create activities.',
    );
  }

  @override
  Future<Activity> updateActivity(Activity activity) {
    throw UnsupportedError(
      'HardcodedActivityRepository is read-only; use SupabaseActivityRepository '
      'to update activities.',
    );
  }

  @override
  Future<void> deleteActivity(String id) {
    throw UnsupportedError(
      'HardcodedActivityRepository is read-only; use SupabaseActivityRepository '
      'to delete activities.',
    );
  }
}

int _bySortOrderThenTitle(Activity a, Activity b) {
  final bySortOrder = a.sortOrder.compareTo(b.sortOrder);
  return bySortOrder != 0 ? bySortOrder : a.title.compareTo(b.title);
}
