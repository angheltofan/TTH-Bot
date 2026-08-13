import 'package:flutter/material.dart';

import 'activity.dart';
import 'activity_repository.dart';

/// Landscape-optimized activity picker for the Android robot.
///
/// Stale-while-revalidate: the very first load blocks (nothing to show
/// yet), but every load after that — returning from an activity, manual
/// refresh — keeps showing the last-known activity list immediately and
/// refreshes quietly in the background. A background refresh failure never
/// blanks the screen; it just leaves the cached list showing. This works
/// because [ActivitySelectorScreen] is the app's `home` route and stays
/// mounted (state preserved) the whole time a [RobotFaceScreen] is pushed
/// on top of it — no external cache/store needed.
class ActivitySelectorScreen extends StatefulWidget {
  const ActivitySelectorScreen({
    super.key,
    required this.repository,
    required this.onActivitySelected,
  });

  final ActivityRepository repository;

  /// Called when the child taps an activity. The screen awaits this (it's
  /// expected to push the robot face screen and resolve once popped) and
  /// reloads activities immediately afterward, so edits made on the web
  /// while a session was running show up without an app restart.
  final Future<void> Function(Activity activity) onActivitySelected;

  @override
  State<ActivitySelectorScreen> createState() => _ActivitySelectorScreenState();
}

class _ActivitySelectorScreenState extends State<ActivitySelectorScreen> {
  /// Whether a load has ever completed (success or failure) — decides
  /// whether the *next* load blocks the screen or refreshes quietly behind
  /// the current cached content.
  bool _hasLoadedOnce = false;

  /// True only while a load is in flight AND nothing is cached yet — the
  /// one case that still shows a full-screen spinner.
  bool _initialLoading = true;

  /// True while any load (including a background refresh) is in flight —
  /// drives the small, non-blocking refresh indicator only.
  bool _refreshing = false;

  List<Activity> _activities = const [];
  String? _initialErrorMessage;

  @override
  void initState() {
    super.initState();
    _load();
  }

  Future<void> _load() async {
    setState(() => _refreshing = true);
    try {
      final activities = await widget.repository.getEnabledActivities();
      if (!mounted) return;
      setState(() {
        _activities = activities;
        _hasLoadedOnce = true;
        _initialLoading = false;
        _initialErrorMessage = null;
        _refreshing = false;
      });
    } catch (e, stackTrace) {
      debugPrint(
        '[ActivitySelector] failed to load activities: $e\n$stackTrace',
      );
      if (!mounted) return;
      setState(() {
        _refreshing = false;
        if (!_hasLoadedOnce) {
          // No cache to fall back on: this failure has to be the visible
          // state. Once we do have a cache, a failed background refresh is
          // silent — the stale list stays on screen, per spec.
          _initialLoading = false;
          _initialErrorMessage = e.toString();
        }
      });
    }
  }

  Future<void> _select(Activity activity) async {
    await widget.onActivitySelected(activity);
    if (!mounted) return;
    _load();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: Colors.black,
      body: SafeArea(
        child: Stack(
          children: [
            Center(child: _buildBody(context)),
            Positioned(top: 8, right: 8, child: _buildRefreshIndicator()),
          ],
        ),
      ),
    );
  }

  Widget _buildBody(BuildContext context) {
    if (_initialLoading) {
      return const CircularProgressIndicator(color: Colors.cyanAccent);
    }
    if (!_hasLoadedOnce) {
      return _ErrorView(message: _initialErrorMessage, onRetry: _load);
    }
    return _ActivityChoices(
      activities: _activities,
      onActivitySelected: _select,
    );
  }

  Widget _buildRefreshIndicator() {
    if (_initialLoading) {
      // The big centered spinner already covers this state — no need for
      // a second one in the corner too.
      return const SizedBox.shrink();
    }
    if (_refreshing) {
      return const Padding(
        padding: EdgeInsets.all(12),
        child: SizedBox(
          width: 20,
          height: 20,
          child: CircularProgressIndicator(
            strokeWidth: 2,
            color: Colors.white70,
          ),
        ),
      );
    }
    return IconButton(
      tooltip: 'Reîncarcă',
      icon: const Icon(Icons.refresh, color: Colors.white70),
      onPressed: _load,
    );
  }
}

class _ActivityChoices extends StatelessWidget {
  const _ActivityChoices({
    required this.activities,
    required this.onActivitySelected,
  });

  final List<Activity> activities;
  final ValueChanged<Activity> onActivitySelected;

  @override
  Widget build(BuildContext context) {
    return Column(
      mainAxisSize: MainAxisSize.min,
      children: [
        const Text(
          'TTH Bot',
          style: TextStyle(
            color: Colors.cyanAccent,
            fontSize: 40,
            fontWeight: FontWeight.bold,
          ),
        ),
        const SizedBox(height: 8),
        Text(
          'Alege o activitate:',
          style: TextStyle(
            color: Colors.white.withValues(alpha: 0.8),
            fontSize: 18,
          ),
        ),
        const SizedBox(height: 32),
        if (activities.isEmpty)
          const Text(
            'Nu există activități disponibile momentan.',
            style: TextStyle(color: Colors.white54),
          )
        else
          Row(
            mainAxisSize: MainAxisSize.min,
            children: [
              for (final activity in activities) ...[
                _ActivityButton(
                  activity: activity,
                  onPressed: () => onActivitySelected(activity),
                ),
                if (activity != activities.last) const SizedBox(width: 24),
              ],
            ],
          ),
      ],
    );
  }
}

class _ActivityButton extends StatelessWidget {
  const _ActivityButton({required this.activity, required this.onPressed});

  final Activity activity;
  final VoidCallback onPressed;

  @override
  Widget build(BuildContext context) {
    return ElevatedButton(
      onPressed: onPressed,
      style: ElevatedButton.styleFrom(
        backgroundColor: Colors.blueGrey.shade900,
        foregroundColor: Colors.cyanAccent,
        padding: const EdgeInsets.symmetric(horizontal: 32, vertical: 20),
        shape: RoundedRectangleBorder(
          borderRadius: BorderRadius.circular(16),
          side: const BorderSide(color: Colors.cyanAccent, width: 1),
        ),
      ),
      child: Text(activity.title, style: const TextStyle(fontSize: 18)),
    );
  }
}

class _ErrorView extends StatelessWidget {
  const _ErrorView({required this.message, required this.onRetry});

  final String? message;
  final VoidCallback onRetry;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: 32),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          const Icon(Icons.cloud_off, color: Colors.white54, size: 40),
          const SizedBox(height: 12),
          const Text(
            'Nu am putut încărca activitățile.',
            style: TextStyle(color: Colors.white70, fontSize: 16),
            textAlign: TextAlign.center,
          ),
          if (message != null) ...[
            const SizedBox(height: 6),
            Text(
              message!,
              style: const TextStyle(color: Colors.white38, fontSize: 12),
              textAlign: TextAlign.center,
              maxLines: 3,
              overflow: TextOverflow.ellipsis,
            ),
          ],
          const SizedBox(height: 16),
          ElevatedButton.icon(
            onPressed: onRetry,
            icon: const Icon(Icons.refresh),
            label: const Text('Încearcă din nou'),
          ),
        ],
      ),
    );
  }
}
