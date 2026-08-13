import 'package:flutter/material.dart';

import '../../core/theme/tth_colors.dart';
import '../activities/activity.dart';
import '../activities/activity_repository.dart';

/// Compact activity-picker overlay opened from the robot face's settings
/// gear (see `RobotHomeScreen`). Deliberately not a full page — the face
/// stays visually behind it. No activity editing here; that stays
/// Web-only, per this task's scope.
///
/// Light/modern card style (white surface, soft shadow, rounded corners) —
/// distinct on purpose from the dark robot-face palette, matching the
/// Phase 5 remediation's reference design rather than a generic Flutter
/// dialog.
///
/// Pops the selected [Activity] when the child chooses one, or `null` on
/// "Închide" / barrier dismiss — the caller (RobotHomeScreen) treats a null
/// result as "resume the current activity unchanged".
class SettingsModal extends StatefulWidget {
  const SettingsModal({
    super.key,
    required this.repository,
    required this.initialActivities,
    required this.currentActivityId,
    this.onActivitiesRefreshed,
  });

  final ActivityRepository repository;

  /// Cached list the parent already has — shown immediately so opening
  /// settings never itself triggers a blocking spinner.
  final List<Activity> initialActivities;
  final String? currentActivityId;

  /// Called with the freshly-fetched list every time a refresh inside this
  /// modal succeeds, so the parent's own cache — used to seed
  /// [initialActivities] the *next* time this modal opens — stays current.
  ///
  /// Without this, a successful refresh only ever updated this modal's own
  /// local state; closing and reopening it reseeded from the parent's
  /// stale, startup-time snapshot, silently discarding the refresh. That
  /// was the actual bug behind "an activity created on the web disappears
  /// again after closing and reopening the settings modal" — not a
  /// hardcoded-repository fallback overwriting Supabase data (there isn't
  /// one; `main.dart` picks the repository once at startup and never
  /// swaps it), just this stale cache never being written back.
  final ValueChanged<List<Activity>>? onActivitiesRefreshed;

  @override
  State<SettingsModal> createState() => _SettingsModalState();
}

class _SettingsModalState extends State<SettingsModal> {
  late List<Activity> _activities;
  bool _refreshing = false;
  String? _refreshError;

  @override
  void initState() {
    super.initState();
    _activities = widget.initialActivities;
  }

  Future<void> _refresh() async {
    setState(() {
      _refreshing = true;
      _refreshError = null;
    });
    try {
      final activities = await widget.repository.getEnabledActivities();
      if (!mounted) return;
      setState(() {
        _activities = activities;
        _refreshing = false;
      });
      widget.onActivitiesRefreshed?.call(activities);
    } catch (e) {
      if (!mounted) return;
      setState(() {
        _refreshing = false;
        // Cached list stays visible; this is just a small inline notice.
        _refreshError = 'Reîncărcare eșuată — lista rămâne cea anterioară.';
      });
    }
  }

  @override
  Widget build(BuildContext context) {
    return Dialog(
      backgroundColor: Colors.white,
      elevation: 12,
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(28)),
      child: ConstrainedBox(
        constraints: const BoxConstraints(maxWidth: 380, maxHeight: 440),
        child: Padding(
          padding: const EdgeInsets.fromLTRB(22, 24, 22, 16),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              const Text(
                'Activitate',
                textAlign: TextAlign.center,
                style: TextStyle(
                  color: TthColors.webTextDark,
                  fontSize: 21,
                  fontWeight: FontWeight.w700,
                ),
              ),
              if (_refreshError != null) ...[
                const SizedBox(height: 6),
                Text(
                  _refreshError!,
                  textAlign: TextAlign.center,
                  style: const TextStyle(
                    color: TthColors.webTextMuted,
                    fontSize: 12,
                  ),
                ),
              ],
              const SizedBox(height: 16),
              Flexible(
                child: _activities.isEmpty
                    ? const Padding(
                        padding: EdgeInsets.symmetric(vertical: 24),
                        child: Text(
                          'Nicio activitate disponibilă',
                          style: TextStyle(color: TthColors.webTextMuted),
                          textAlign: TextAlign.center,
                        ),
                      )
                    : ListView.separated(
                        shrinkWrap: true,
                        itemCount: _activities.length,
                        separatorBuilder: (_, _) => const SizedBox(height: 10),
                        itemBuilder: (context, index) {
                          final activity = _activities[index];
                          return _ActivityTile(
                            activity: activity,
                            selected: activity.id == widget.currentActivityId,
                            onTap: () => Navigator.of(context).pop(activity),
                          );
                        },
                      ),
              ),
              const SizedBox(height: 14),
              Row(
                mainAxisAlignment: MainAxisAlignment.spaceBetween,
                children: [
                  Flexible(
                    child: _RefreshAction(
                      refreshing: _refreshing,
                      onPressed: _refresh,
                    ),
                  ),
                  const SizedBox(width: 8),
                  FilledButton(
                    style: FilledButton.styleFrom(
                      backgroundColor: TthColors.brandBlue,
                      shape: RoundedRectangleBorder(
                        borderRadius: BorderRadius.circular(14),
                      ),
                      padding: const EdgeInsets.symmetric(
                        horizontal: 18,
                        vertical: 10,
                      ),
                      minimumSize: Size.zero,
                      tapTargetSize: MaterialTapTargetSize.shrinkWrap,
                    ),
                    onPressed: () => Navigator.of(context).pop(),
                    child: const Text('Închide'),
                  ),
                ],
              ),
            ],
          ),
        ),
      ),
    );
  }
}

class _RefreshAction extends StatelessWidget {
  const _RefreshAction({required this.refreshing, required this.onPressed});

  final bool refreshing;
  final VoidCallback onPressed;

  @override
  Widget build(BuildContext context) {
    if (refreshing) {
      return const Padding(
        padding: EdgeInsets.symmetric(horizontal: 4, vertical: 12),
        child: SizedBox(
          width: 16,
          height: 16,
          child: CircularProgressIndicator(
            strokeWidth: 2,
            color: TthColors.brandBlue,
          ),
        ),
      );
    }
    return TextButton.icon(
      onPressed: onPressed,
      style: TextButton.styleFrom(
        foregroundColor: TthColors.brandBlue,
        padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 8),
        minimumSize: Size.zero,
        tapTargetSize: MaterialTapTargetSize.shrinkWrap,
      ),
      icon: const Icon(Icons.refresh, size: 16),
      label: const Text('Actualizează', overflow: TextOverflow.ellipsis),
    );
  }
}

class _ActivityTile extends StatelessWidget {
  const _ActivityTile({
    required this.activity,
    required this.selected,
    required this.onTap,
  });

  final Activity activity;
  final bool selected;
  final VoidCallback onTap;

  static String _typeLabel(ActivityType type) => switch (type) {
    ActivityType.lesson => 'Lecție',
    ActivityType.game => 'Joc',
    ActivityType.conversation => 'Conversație',
  };

  static String _modeLabel(InteractionMode mode) => switch (mode) {
    InteractionMode.pushToTalk => 'Push-to-talk',
    InteractionMode.freeConversation => 'Hands-free',
  };

  static IconData _typeIcon(ActivityType type) => switch (type) {
    ActivityType.lesson => Icons.menu_book_rounded,
    ActivityType.game => Icons.sports_esports_rounded,
    ActivityType.conversation => Icons.chat_bubble_rounded,
  };

  @override
  Widget build(BuildContext context) {
    return Material(
      color: Colors.transparent,
      child: InkWell(
        onTap: onTap,
        borderRadius: BorderRadius.circular(16),
        child: Container(
          padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 10),
          decoration: BoxDecoration(
            color: selected
                ? TthColors.brandBlue.withValues(alpha: 0.08)
                : const Color(0xFFF6F8FA),
            borderRadius: BorderRadius.circular(16),
            border: Border.all(
              color: selected
                  ? TthColors.brandBlue.withValues(alpha: 0.35)
                  : Colors.transparent,
            ),
          ),
          child: Row(
            children: [
              Container(
                width: 38,
                height: 38,
                decoration: BoxDecoration(
                  color: TthColors.brandOrange.withValues(alpha: 0.14),
                  shape: BoxShape.circle,
                ),
                child: Icon(
                  _typeIcon(activity.type),
                  size: 18,
                  color: TthColors.brandOrange,
                ),
              ),
              const SizedBox(width: 12),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(
                      activity.title,
                      style: const TextStyle(
                        color: TthColors.webTextDark,
                        fontSize: 15,
                        fontWeight: FontWeight.w600,
                      ),
                    ),
                    const SizedBox(height: 2),
                    Text(
                      '${_typeLabel(activity.type)} · ${_modeLabel(activity.interactionMode)}',
                      style: const TextStyle(
                        color: TthColors.webTextMuted,
                        fontSize: 12,
                      ),
                    ),
                  ],
                ),
              ),
              const SizedBox(width: 8),
              _SelectionBadge(selected: selected),
            ],
          ),
        ),
      ),
    );
  }
}

class _SelectionBadge extends StatelessWidget {
  const _SelectionBadge({required this.selected});

  final bool selected;

  @override
  Widget build(BuildContext context) {
    if (selected) {
      return Container(
        width: 24,
        height: 24,
        decoration: const BoxDecoration(
          color: TthColors.brandBlue,
          shape: BoxShape.circle,
        ),
        child: const Icon(Icons.check, size: 15, color: Colors.white),
      );
    }
    return Container(
      width: 24,
      height: 24,
      decoration: BoxDecoration(
        shape: BoxShape.circle,
        border: Border.all(color: const Color(0xFFD5DEE6), width: 1.5),
      ),
    );
  }
}
