import 'package:flutter/material.dart';

import '../../../core/theme/tth_colors.dart';
import '../activity.dart';
import '../activity_repository.dart';
import 'activity_form_screen.dart';

enum _LoadState { loading, loaded, error }

const double _pageMaxWidth = 960;

/// Desktop/browser-oriented activity management screen — this is what
/// `flutter run -d chrome` shows, never the robot face. A single centered
/// column (Phase 5, Part B): no sidebar, no scattered navigation, just the
/// Tales & Tech Hub header, a search box and the activity list.
class ActivityWebApp extends StatefulWidget {
  const ActivityWebApp({super.key, required this.repository});

  final ActivityRepository repository;

  @override
  State<ActivityWebApp> createState() => _ActivityWebAppState();
}

class _ActivityWebAppState extends State<ActivityWebApp> {
  _LoadState _state = _LoadState.loading;
  List<Activity> _activities = const [];
  String? _errorMessage;
  final TextEditingController _searchController = TextEditingController();
  String _query = '';

  @override
  void initState() {
    super.initState();
    _searchController.addListener(() {
      setState(() => _query = _searchController.text.trim().toLowerCase());
    });
    _load();
  }

  @override
  void dispose() {
    _searchController.dispose();
    super.dispose();
  }

  Future<void> _load() async {
    setState(() => _state = _LoadState.loading);
    try {
      final activities = await widget.repository.getAllActivities();
      if (!mounted) return;
      setState(() {
        _activities = activities;
        _state = _LoadState.loaded;
      });
    } catch (e, stackTrace) {
      debugPrint('[ActivityWebApp] failed to load activities: $e\n$stackTrace');
      if (!mounted) return;
      setState(() {
        _errorMessage = e.toString();
        _state = _LoadState.error;
      });
    }
  }

  Future<void> _openForm({Activity? existing}) async {
    final saved = await Navigator.of(context).push<bool>(
      MaterialPageRoute(
        builder: (_) => ActivityFormScreen(
          repository: widget.repository,
          existing: existing,
        ),
      ),
    );
    if (saved == true) _load();
  }

  Future<void> _delete(Activity activity) async {
    final confirmed = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(16)),
        title: Text('Ștergi activitatea «${activity.title}»?'),
        content: const Text('Această acțiune nu poate fi anulată.'),
        actions: [
          TextButton(
            onPressed: () => Navigator.of(context).pop(false),
            child: const Text('Anulează'),
          ),
          FilledButton(
            style: FilledButton.styleFrom(backgroundColor: Colors.red.shade600),
            onPressed: () => Navigator.of(context).pop(true),
            child: const Text('Șterge'),
          ),
        ],
      ),
    );
    if (confirmed != true) return;

    try {
      await widget.repository.deleteActivity(activity.id);
      _load();
    } catch (e) {
      if (!mounted) return;
      ScaffoldMessenger.of(
        context,
      ).showSnackBar(SnackBar(content: Text('Eroare la ștergere: $e')));
    }
  }

  List<Activity> get _filteredActivities {
    if (_query.isEmpty) return _activities;
    return _activities
        .where((a) => a.title.toLowerCase().contains(_query))
        .toList();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      body: SafeArea(
        child: Stack(
          children: [
            Positioned(
              top: 8,
              right: 8,
              child: IconButton(
                tooltip: 'Reîncarcă',
                icon: const Icon(Icons.refresh),
                onPressed: _state == _LoadState.loading ? null : _load,
              ),
            ),
            // The scrollable must span the FULL viewport width/height —
            // not just the centered content column — so the mouse wheel
            // scrolls the page no matter where the pointer is (including
            // the empty side margins on a wide desktop window). Centering
            // and the max-width constraint happen *inside* the
            // scrollable's child, not around it.
            SingleChildScrollView(
              child: Center(
                child: ConstrainedBox(
                  constraints: const BoxConstraints(maxWidth: _pageMaxWidth),
                  child: Padding(
                    padding: const EdgeInsets.symmetric(
                      horizontal: 24,
                      vertical: 32,
                    ),
                    child: Column(
                      children: [
                        const _WebHeader(),
                        const SizedBox(height: 40),
                        _TitleSection(onCreate: () => _openForm()),
                        const SizedBox(height: 28),
                        _SearchField(controller: _searchController),
                        const SizedBox(height: 28),
                        _buildBody(context),
                      ],
                    ),
                  ),
                ),
              ),
            ),
          ],
        ),
      ),
    );
  }

  Widget _buildBody(BuildContext context) {
    switch (_state) {
      case _LoadState.loading:
        return const Padding(
          padding: EdgeInsets.only(top: 40),
          child: CircularProgressIndicator(),
        );
      case _LoadState.error:
        return _ErrorView(message: _errorMessage, onRetry: _load);
      case _LoadState.loaded:
        final activities = _filteredActivities;
        if (_activities.isEmpty) {
          return const _EmptyView();
        }
        if (activities.isEmpty) {
          return const Padding(
            padding: EdgeInsets.only(top: 24),
            child: Text(
              'Nicio activitate nu corespunde căutării.',
              style: TextStyle(color: TthColors.webTextMuted),
            ),
          );
        }
        return Column(
          children: [
            for (final activity in activities) ...[
              _ActivityCard(
                activity: activity,
                onEdit: () => _openForm(existing: activity),
                onDelete: () => _delete(activity),
              ),
              const SizedBox(height: 14),
            ],
          ],
        );
    }
  }
}

class _WebHeader extends StatelessWidget {
  const _WebHeader();

  @override
  Widget build(BuildContext context) {
    return Column(
      children: [
        Image.asset(
          'assets/branding/tales_tech_hub_logo.png',
          height: 100,
          errorBuilder: (context, error, stackTrace) => const Text(
            'Tales & Tech Hub',
            style: TextStyle(
              color: TthColors.brandBlue,
              fontSize: 20,
              fontWeight: FontWeight.w700,
            ),
          ),
        ),
      ],
    );
  }
}

class _TitleSection extends StatelessWidget {
  const _TitleSection({required this.onCreate});

  final VoidCallback onCreate;

  @override
  Widget build(BuildContext context) {
    return Column(
      children: [
        const Text(
          'TTH Bot – Activități',
          textAlign: TextAlign.center,
          style: TextStyle(
            color: TthColors.webTextDark,
            fontSize: 24,
            fontWeight: FontWeight.w600,
          ),
        ),
        const SizedBox(height: 8),
        Text(
          'Creează și configurează activitățile robotului.',
          textAlign: TextAlign.center,
          style: TextStyle(color: TthColors.webTextMuted, fontSize: 15),
        ),
        const SizedBox(height: 20),
        FilledButton.icon(
          style: FilledButton.styleFrom(
            backgroundColor: TthColors.brandOrange,
            foregroundColor: Colors.white,
          ),
          onPressed: onCreate,
          icon: const Icon(Icons.add),
          label: const Text('Activitate nouă'),
        ),
      ],
    );
  }
}

class _SearchField extends StatelessWidget {
  const _SearchField({required this.controller});

  final TextEditingController controller;

  @override
  Widget build(BuildContext context) {
    return Center(
      child: ConstrainedBox(
        constraints: const BoxConstraints(maxWidth: 600),
        child: TextField(
          controller: controller,
          decoration: InputDecoration(
            hintText: 'Caută o activitate...',
            prefixIcon: const Icon(Icons.search),
          ),
        ),
      ),
    );
  }
}

class _ActivityCard extends StatelessWidget {
  const _ActivityCard({
    required this.activity,
    required this.onEdit,
    required this.onDelete,
  });

  final Activity activity;
  final VoidCallback onEdit;
  final VoidCallback onDelete;

  @override
  Widget build(BuildContext context) {
    return Card(
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 24, vertical: 22),
        child: Column(
          children: [
            Text(
              activity.title,
              textAlign: TextAlign.center,
              style: const TextStyle(
                fontSize: 20,
                fontWeight: FontWeight.w600,
                color: TthColors.webTextDark,
              ),
            ),
            const SizedBox(height: 8),
            _StatusChip(enabled: activity.enabled),
            const SizedBox(height: 10),
            Text(
              '${activity.type.label} · ${activity.interactionMode.label}',
              textAlign: TextAlign.center,
              style: const TextStyle(
                color: TthColors.webTextMuted,
                fontSize: 13,
              ),
            ),
            if (activity.participants.isNotEmpty) ...[
              const SizedBox(height: 8),
              Text(
                _participantsSummary(activity.participants),
                textAlign: TextAlign.center,
                style: const TextStyle(
                  color: TthColors.webTextMuted,
                  fontSize: 13,
                ),
              ),
            ],
            const SizedBox(height: 16),
            Row(
              mainAxisAlignment: MainAxisAlignment.center,
              children: [
                OutlinedButton.icon(
                  onPressed: onEdit,
                  icon: const Icon(Icons.edit_outlined, size: 18),
                  label: const Text('Editează'),
                ),
                const SizedBox(width: 12),
                OutlinedButton.icon(
                  style: OutlinedButton.styleFrom(
                    foregroundColor: Colors.red.shade600,
                    side: BorderSide(color: Colors.red.shade200),
                  ),
                  onPressed: onDelete,
                  icon: const Icon(Icons.delete_outline, size: 18),
                  label: const Text('Șterge'),
                ),
              ],
            ),
          ],
        ),
      ),
    );
  }

  static String _participantsSummary(List<String> participants) {
    const maxShown = 4;
    if (participants.length <= maxShown) return participants.join(' · ');
    final shown = participants.take(maxShown).join(' · ');
    return '$shown · +${participants.length - maxShown}';
  }
}

class _StatusChip extends StatelessWidget {
  const _StatusChip({required this.enabled});

  final bool enabled;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 3),
      decoration: BoxDecoration(
        color: enabled
            ? TthColors.brandBlue.withValues(alpha: 0.1)
            : Colors.grey.withValues(alpha: 0.15),
        borderRadius: BorderRadius.circular(12),
      ),
      child: Text(
        enabled ? 'ACTIVĂ' : 'DEZACTIVATĂ',
        style: TextStyle(
          fontSize: 11,
          fontWeight: FontWeight.w700,
          letterSpacing: 0.4,
          color: enabled ? TthColors.brandBlue : Colors.grey.shade600,
        ),
      ),
    );
  }
}

class _EmptyView extends StatelessWidget {
  const _EmptyView();

  @override
  Widget build(BuildContext context) {
    return const Padding(
      padding: EdgeInsets.only(top: 40),
      child: Text(
        'Nu există încă nicio activitate.',
        style: TextStyle(color: TthColors.webTextMuted),
      ),
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
      padding: const EdgeInsets.only(top: 24),
      child: Column(
        children: [
          Icon(Icons.cloud_off, size: 40, color: TthColors.webTextMuted),
          const SizedBox(height: 12),
          const Text(
            'Nu am putut încărca activitățile.',
            style: TextStyle(color: TthColors.webTextDark),
          ),
          if (message != null) ...[
            const SizedBox(height: 6),
            Text(
              message!,
              style: const TextStyle(
                color: TthColors.webTextMuted,
                fontSize: 12,
              ),
              textAlign: TextAlign.center,
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
