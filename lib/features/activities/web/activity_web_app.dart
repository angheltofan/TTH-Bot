import 'package:flutter/material.dart';

import '../../../core/theme/tth_colors.dart';
import '../activity.dart';
import '../activity_failure.dart';
import '../activity_repository.dart';
import 'activity_form_screen.dart';

enum _LoadState { loading, loaded, error }

/// Which activities the list shows, filtered locally over what is already
/// loaded — no extra query, no schema change.
enum ActivityStatusFilter { all, enabled, disabled }

extension ActivityStatusFilterLabel on ActivityStatusFilter {
  String get label => switch (this) {
    ActivityStatusFilter.all => 'Toate activitățile',
    ActivityStatusFilter.enabled => 'Active',
    ActivityStatusFilter.disabled => 'Inactive',
  };
}

/// `1 activitate`, otherwise `<count> activități`.
String activityCountLabel(int count) =>
    count == 1 ? '1 activitate' : '$count activități';

const double _pageMaxWidth = 1120;
const double _threeColumnWidth = 1000;
const double _twoColumnWidth = 660;
const double _compactWidth = 600;

const double _gridSpacing = 20;

/// Administration dashboard for the robot's activities: a compact top
/// header, the activity list as cards, search and a status filter.
///
/// Shown only behind `AdminAuthGate`. Errors are shown as fixed messages
/// ([activityFailureMessage]); raw repository errors are never displayed
/// or logged.
class ActivityWebApp extends StatefulWidget {
  const ActivityWebApp({
    super.key,
    required this.repository,
    this.onLogout,
    this.onSessionExpired,
  });

  final ActivityRepository repository;

  /// Shows the "Deconectare" button when set.
  final VoidCallback? onLogout;

  /// Called when a request fails because the session is no longer valid.
  final VoidCallback? onSessionExpired;

  @override
  State<ActivityWebApp> createState() => _ActivityWebAppState();
}

class _ActivityWebAppState extends State<ActivityWebApp> {
  _LoadState _state = _LoadState.loading;
  List<Activity> _activities = const [];
  String? _errorMessage;
  final TextEditingController _searchController = TextEditingController();
  String _query = '';
  ActivityStatusFilter _filter = ActivityStatusFilter.all;

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
    } catch (e) {
      final failure = activityFailureOf(e);
      // Only the failure kind: error text can carry server details.
      debugPrint(
        '[ActivityWebApp] failed to load activities (${failure.name})',
      );
      if (!mounted) return;
      setState(() {
        _errorMessage = activityFailureMessage(failure, ActivityAction.load);
        _state = _LoadState.error;
      });
      if (failure == ActivityFailure.sessionExpired) {
        widget.onSessionExpired?.call();
      }
    }
  }

  Future<void> _openForm({Activity? existing}) async {
    final saved = await Navigator.of(context).push<bool>(
      MaterialPageRoute(
        builder: (_) => ActivityFormScreen(
          repository: widget.repository,
          existing: existing,
          onSessionExpired: widget.onSessionExpired,
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
      final failure = activityFailureOf(e);
      if (failure == ActivityFailure.sessionExpired) {
        widget.onSessionExpired?.call();
        return;
      }
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          content: Text(activityFailureMessage(failure, ActivityAction.delete)),
        ),
      );
      // A delete that changed nothing may mean the list is stale.
      if (failure == ActivityFailure.notChanged) _load();
    }
  }

  List<Activity> get _visibleActivities {
    return _activities.where((activity) {
      final matchesQuery =
          _query.isEmpty || activity.title.toLowerCase().contains(_query);
      final matchesStatus = switch (_filter) {
        ActivityStatusFilter.all => true,
        ActivityStatusFilter.enabled => activity.enabled,
        ActivityStatusFilter.disabled => !activity.enabled,
      };
      return matchesQuery && matchesStatus;
    }).toList();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: TthColors.webDashboardBackground,
      body: SafeArea(
        child: Column(
          children: [
            _DashboardHeader(
              onRefresh: _state == _LoadState.loading ? null : _load,
              onLogout: widget.onLogout,
            ),
            Expanded(
              child: SingleChildScrollView(
                child: Center(
                  child: ConstrainedBox(
                    constraints: const BoxConstraints(maxWidth: _pageMaxWidth),
                    child: LayoutBuilder(
                      builder: (context, constraints) {
                        final compact = constraints.maxWidth < _compactWidth;
                        return Padding(
                          padding: EdgeInsets.symmetric(
                            horizontal: compact ? 16 : 24,
                            vertical: compact ? 20 : 28,
                          ),
                          child: Column(
                            crossAxisAlignment: CrossAxisAlignment.stretch,
                            children: [
                              _TitleSection(
                                count: _activities.length,
                                showCount: _state == _LoadState.loaded,
                                compact: compact,
                                onCreate: () => _openForm(),
                              ),
                              SizedBox(height: compact ? 20 : 24),
                              _SearchAndFilterBar(
                                controller: _searchController,
                                filter: _filter,
                                compact: compact,
                                onFilterChanged: (value) =>
                                    setState(() => _filter = value),
                              ),
                              SizedBox(height: compact ? 20 : 24),
                              _buildBody(context),
                            ],
                          ),
                        );
                      },
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
        return const _MessagePanel(
          icon: null,
          message: 'Se încarcă activitățile...',
        );
      case _LoadState.error:
        return _MessagePanel(
          icon: Icons.cloud_off,
          message: _errorMessage ?? 'Nu am putut încărca activitățile.',
          action: FilledButton.icon(
            onPressed: _load,
            icon: const Icon(Icons.refresh),
            label: const Text('Încearcă din nou'),
          ),
        );
      case _LoadState.loaded:
        if (_activities.isEmpty) {
          return const _MessagePanel(
            icon: Icons.inbox_outlined,
            message: 'Nu există încă nicio activitate.',
          );
        }
        final activities = _visibleActivities;
        if (activities.isEmpty) {
          return const _MessagePanel(
            icon: Icons.search_off,
            message: 'Nicio activitate nu corespunde căutării.',
          );
        }
        return _ActivityGrid(
          activities: activities,
          onEdit: (activity) => _openForm(existing: activity),
          onDelete: _delete,
        );
    }
  }
}

/// Full-width white bar: brand on the left, the two page actions on the
/// right. Nothing floats over the page content.
class _DashboardHeader extends StatelessWidget {
  const _DashboardHeader({required this.onRefresh, required this.onLogout});

  final VoidCallback? onRefresh;
  final VoidCallback? onLogout;

  @override
  Widget build(BuildContext context) {
    final compact = MediaQuery.sizeOf(context).width < _compactWidth;
    return Container(
      decoration: const BoxDecoration(
        color: TthColors.white,
        border: Border(bottom: BorderSide(color: TthColors.webCardBorder)),
        boxShadow: [
          BoxShadow(
            color: TthColors.webShadow,
            blurRadius: 8,
            offset: Offset(0, 1),
          ),
        ],
      ),
      padding: EdgeInsets.symmetric(
        horizontal: compact ? 12 : 24,
        vertical: 10,
      ),
      child: Row(
        children: [
          // Expanded, not Flexible + Spacer: the brand keeps the space the
          // actions do not need, so the title only truncates when it really
          // does not fit.
          Expanded(
            child: Row(
              mainAxisSize: MainAxisSize.min,
              children: [
                // The brand symbol without the wordmark: at header size the
                // lockup's tiny "Tales & Tech HUB" text is unreadable, and
                // the title beside it already names the application.
                Image.asset(
                  'assets/branding/logo.png',
                  height: compact ? 36 : 52,
                  fit: BoxFit.contain,
                  filterQuality: FilterQuality.high,
                  isAntiAlias: true,
                  semanticLabel: 'Tales & Tech Hub',
                  errorBuilder: (context, error, stackTrace) => const Text(
                    'Tales & Tech Hub',
                    style: TextStyle(
                      color: TthColors.brandBlue,
                      fontSize: 14,
                      fontWeight: FontWeight.w700,
                    ),
                  ),
                ),
                const SizedBox(width: 12),
                Container(
                  width: 1,
                  height: compact ? 22 : 28,
                  color: TthColors.webCardBorder,
                ),
                const SizedBox(width: 12),
                Flexible(
                  child: Text(
                    'Administrare TTH Bot',
                    overflow: TextOverflow.ellipsis,
                    style: TextStyle(
                      color: TthColors.webTextDark,
                      fontSize: compact ? 14 : 16,
                      fontWeight: FontWeight.w600,
                    ),
                  ),
                ),
              ],
            ),
          ),
          const SizedBox(width: 12),
          _HeaderAction(
            icon: Icons.refresh,
            label: 'Actualizează',
            compact: compact,
            onPressed: onRefresh,
          ),
          if (onLogout != null) ...[
            SizedBox(width: compact ? 0 : 4),
            _HeaderAction(
              icon: Icons.logout,
              label: 'Deconectare',
              compact: compact,
              onPressed: onLogout,
            ),
          ],
        ],
      ),
    );
  }
}

/// Icon + label where there is room, icon only on a phone. Both keep the
/// same tooltip and semantic label, and both are keyboard reachable.
class _HeaderAction extends StatelessWidget {
  const _HeaderAction({
    required this.icon,
    required this.label,
    required this.compact,
    required this.onPressed,
  });

  final IconData icon;
  final String label;
  final bool compact;
  final VoidCallback? onPressed;

  @override
  Widget build(BuildContext context) {
    if (compact) {
      return IconButton(
        tooltip: label,
        icon: Icon(icon),
        color: TthColors.webTextDark,
        onPressed: onPressed,
        constraints: const BoxConstraints(minWidth: 44, minHeight: 44),
      );
    }
    return Tooltip(
      message: label,
      child: TextButton.icon(
        onPressed: onPressed,
        icon: Icon(icon, size: 18),
        label: Text(label),
        style: TextButton.styleFrom(
          foregroundColor: TthColors.webTextDark,
          padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 14),
        ),
      ),
    );
  }
}

class _TitleSection extends StatelessWidget {
  const _TitleSection({
    required this.count,
    required this.showCount,
    required this.compact,
    required this.onCreate,
  });

  final int count;
  final bool showCount;
  final bool compact;
  final VoidCallback onCreate;

  @override
  Widget build(BuildContext context) {
    final heading = Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      mainAxisSize: MainAxisSize.min,
      children: [
        Text(
          'Activități TTH Bot',
          style: TextStyle(
            color: TthColors.webTextDark,
            fontSize: compact ? 22 : 26,
            fontWeight: FontWeight.w700,
          ),
        ),
        const SizedBox(height: 6),
        const Text(
          'Creează și configurează activitățile robotului.',
          style: TextStyle(color: TthColors.webTextMuted, fontSize: 15),
        ),
        if (showCount) ...[
          const SizedBox(height: 12),
          _CountPill(count: count),
        ],
      ],
    );

    final createButton = FilledButton.icon(
      style: FilledButton.styleFrom(
        backgroundColor: TthColors.brandOrange,
        foregroundColor: TthColors.white,
        minimumSize: const Size(0, 48),
      ),
      onPressed: onCreate,
      icon: const Icon(Icons.add),
      label: const Text('Activitate nouă'),
    );

    if (compact) {
      return Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [heading, const SizedBox(height: 16), createButton],
      );
    }
    return Row(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Expanded(child: heading),
        const SizedBox(width: 24),
        Padding(padding: const EdgeInsets.only(top: 4), child: createButton),
      ],
    );
  }
}

class _CountPill extends StatelessWidget {
  const _CountPill({required this.count});

  final int count;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 5),
      decoration: BoxDecoration(
        color: TthColors.brandBlue.withValues(alpha: 0.08),
        borderRadius: BorderRadius.circular(999),
      ),
      child: Text(
        activityCountLabel(count),
        style: const TextStyle(
          color: TthColors.brandBlue,
          fontSize: 13,
          fontWeight: FontWeight.w600,
        ),
      ),
    );
  }
}

class _SearchAndFilterBar extends StatelessWidget {
  const _SearchAndFilterBar({
    required this.controller,
    required this.filter,
    required this.compact,
    required this.onFilterChanged,
  });

  final TextEditingController controller;
  final ActivityStatusFilter filter;
  final bool compact;
  final ValueChanged<ActivityStatusFilter> onFilterChanged;

  @override
  Widget build(BuildContext context) {
    final search = TextField(
      controller: controller,
      decoration: const InputDecoration(
        hintText: 'Caută o activitate...',
        prefixIcon: Icon(Icons.search),
      ),
    );
    final statusFilter = _StatusFilterField(
      filter: filter,
      onChanged: onFilterChanged,
    );

    if (compact) {
      return Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [search, const SizedBox(height: 12), statusFilter],
      );
    }
    return Row(
      children: [
        Expanded(child: search),
        const SizedBox(width: 12),
        SizedBox(width: 230, child: statusFilter),
      ],
    );
  }
}

class _StatusFilterField extends StatelessWidget {
  const _StatusFilterField({required this.filter, required this.onChanged});

  final ActivityStatusFilter filter;
  final ValueChanged<ActivityStatusFilter> onChanged;

  @override
  Widget build(BuildContext context) {
    return DropdownButtonFormField<ActivityStatusFilter>(
      initialValue: filter,
      isExpanded: true,
      decoration: const InputDecoration(
        prefixIcon: Icon(Icons.filter_list),
        contentPadding: EdgeInsets.symmetric(horizontal: 12, vertical: 14),
      ),
      items: [
        for (final value in ActivityStatusFilter.values)
          DropdownMenuItem(value: value, child: Text(value.label)),
      ],
      onChanged: (value) => onChanged(value ?? filter),
    );
  }
}

/// Three columns on a wide desktop, two at medium width, one on a phone.
/// Every card in a row has the same height.
class _ActivityGrid extends StatelessWidget {
  const _ActivityGrid({
    required this.activities,
    required this.onEdit,
    required this.onDelete,
  });

  final List<Activity> activities;
  final ValueChanged<Activity> onEdit;
  final ValueChanged<Activity> onDelete;

  static int columnsFor(double width) {
    if (width >= _threeColumnWidth) return 3;
    if (width >= _twoColumnWidth) return 2;
    return 1;
  }

  @override
  Widget build(BuildContext context) {
    return LayoutBuilder(
      builder: (context, constraints) {
        final columns = columnsFor(constraints.maxWidth);
        final rows = <Widget>[];
        for (var start = 0; start < activities.length; start += columns) {
          final row = activities.skip(start).take(columns).toList();
          if (rows.isNotEmpty) rows.add(const SizedBox(height: _gridSpacing));
          rows.add(
            // IntrinsicHeight, not a fixed grid extent: every card in the
            // row is as tall as the tallest one, whatever the text wraps to,
            // so no card can overflow.
            IntrinsicHeight(
              child: Row(
                crossAxisAlignment: CrossAxisAlignment.stretch,
                children: [
                  for (var column = 0; column < columns; column++) ...[
                    if (column > 0) const SizedBox(width: _gridSpacing),
                    Expanded(
                      child: column < row.length
                          ? _ActivityCard(
                              activity: row[column],
                              onEdit: () => onEdit(row[column]),
                              onDelete: () => onDelete(row[column]),
                            )
                          : const SizedBox.shrink(),
                    ),
                  ],
                ],
              ),
            ),
          );
        }
        return Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: rows,
        );
      },
    );
  }
}

class _ActivityCard extends StatefulWidget {
  const _ActivityCard({
    required this.activity,
    required this.onEdit,
    required this.onDelete,
  });

  final Activity activity;
  final VoidCallback onEdit;
  final VoidCallback onDelete;

  @override
  State<_ActivityCard> createState() => _ActivityCardState();
}

class _ActivityCardState extends State<_ActivityCard> {
  bool _hovered = false;

  @override
  Widget build(BuildContext context) {
    final activity = widget.activity;
    return MouseRegion(
      onEnter: (_) => setState(() => _hovered = true),
      onExit: (_) => setState(() => _hovered = false),
      child: Card(
        elevation: _hovered ? 4 : 0,
        shadowColor: TthColors.webShadow,
        margin: EdgeInsets.zero,
        child: Padding(
          padding: const EdgeInsets.fromLTRB(20, 18, 20, 14),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Row(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Expanded(
                    child: Text(
                      activity.title,
                      maxLines: 2,
                      overflow: TextOverflow.ellipsis,
                      style: const TextStyle(
                        fontSize: 18,
                        fontWeight: FontWeight.w600,
                        color: TthColors.webTextDark,
                      ),
                    ),
                  ),
                  const SizedBox(width: 10),
                  _StatusChip(enabled: activity.enabled),
                ],
              ),
              const SizedBox(height: 12),
              Wrap(
                spacing: 8,
                runSpacing: 8,
                children: [
                  _MetaChip(label: activity.type.label),
                  if (activity.participants.isNotEmpty)
                    _MetaChip(
                      icon: Icons.group_outlined,
                      label: _participantsLabel(activity.participants.length),
                    ),
                ],
              ),
              const SizedBox(height: 18),
              const Spacer(),
              Row(
                children: [
                  Expanded(
                    child: OutlinedButton.icon(
                      onPressed: widget.onEdit,
                      icon: const Icon(Icons.edit_outlined, size: 18),
                      label: const Text('Editează'),
                      style: OutlinedButton.styleFrom(
                        minimumSize: const Size(0, 44),
                        padding: const EdgeInsets.symmetric(horizontal: 12),
                      ),
                    ),
                  ),
                  const SizedBox(width: 10),
                  Expanded(
                    child: OutlinedButton.icon(
                      onPressed: widget.onDelete,
                      icon: const Icon(Icons.delete_outline, size: 18),
                      label: const Text('Șterge'),
                      style: OutlinedButton.styleFrom(
                        foregroundColor: Colors.red.shade600,
                        side: BorderSide(color: Colors.red.shade200),
                        minimumSize: const Size(0, 44),
                        padding: const EdgeInsets.symmetric(horizontal: 12),
                      ),
                    ),
                  ),
                ],
              ),
            ],
          ),
        ),
      ),
    );
  }

  static String _participantsLabel(int count) =>
      count == 1 ? '1 participant' : '$count participanți';
}

class _MetaChip extends StatelessWidget {
  const _MetaChip({required this.label, this.icon});

  final String label;
  final IconData? icon;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 5),
      decoration: BoxDecoration(
        color: TthColors.webDashboardBackground,
        borderRadius: BorderRadius.circular(8),
        border: Border.all(color: TthColors.webCardBorder),
      ),
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          if (icon != null) ...[
            Icon(icon, size: 14, color: TthColors.webTextMuted),
            const SizedBox(width: 6),
          ],
          Text(
            label,
            style: const TextStyle(
              color: TthColors.webTextMuted,
              fontSize: 12.5,
              fontWeight: FontWeight.w500,
            ),
          ),
        ],
      ),
    );
  }
}

class _StatusChip extends StatelessWidget {
  const _StatusChip({required this.enabled});

  final bool enabled;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 4),
      decoration: BoxDecoration(
        color: enabled
            ? TthColors.brandBlue.withValues(alpha: 0.1)
            : const Color(0xFFEDF1F5),
        borderRadius: BorderRadius.circular(999),
      ),
      child: Text(
        enabled ? 'ACTIVĂ' : 'INACTIVĂ',
        style: TextStyle(
          fontSize: 11,
          fontWeight: FontWeight.w700,
          letterSpacing: 0.4,
          color: enabled ? TthColors.brandBlue : TthColors.webTextMuted,
        ),
      ),
    );
  }
}

/// One presentation for loading, empty, no-results and error states.
class _MessagePanel extends StatelessWidget {
  const _MessagePanel({required this.icon, required this.message, this.action});

  final IconData? icon;
  final String message;
  final Widget? action;

  @override
  Widget build(BuildContext context) {
    return Card(
      margin: EdgeInsets.zero,
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 24, vertical: 40),
        child: Column(
          children: [
            if (icon == null)
              const SizedBox(
                width: 28,
                height: 28,
                child: CircularProgressIndicator(strokeWidth: 3),
              )
            else
              Icon(icon, size: 34, color: TthColors.webTextMuted),
            const SizedBox(height: 14),
            Text(
              message,
              textAlign: TextAlign.center,
              style: const TextStyle(color: TthColors.webTextDark),
            ),
            if (action != null) ...[const SizedBox(height: 18), action!],
          ],
        ),
      ),
    );
  }
}
