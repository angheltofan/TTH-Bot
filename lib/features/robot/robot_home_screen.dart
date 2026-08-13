import 'dart:async';

import 'package:flutter/foundation.dart' show kDebugMode;
import 'package:flutter/material.dart';
import 'package:wakelock_plus/wakelock_plus.dart';

import '../../core/theme/tth_colors.dart';
import '../activities/activity.dart';
import '../activities/activity_repository.dart';
import '../activities/prompt_composer.dart';
import '../voice/gemini_auth.dart';
import '../voice/voice_session_controller.dart';
import 'robot_expression.dart';
import 'robot_face.dart';
import 'settings_modal.dart';

/// Shared translucent-dark-circle styling for the two permanent face
/// controls (gear, start/stop) — a subtle fill plus a faint border for
/// definition against the dark background, matching the reference image's
/// discreet round corner buttons.
const Color _kButtonFill = Color(0x1AFFFFFF); // white @ ~10% alpha
const BorderSide _kButtonBorder = BorderSide(
  color: Color(0x1FFFFFFF), // white @ ~12% alpha
);

/// The single persistent Android screen (Phase 5, Part C): just the robot
/// face, plus a small settings gear that opens a compact activity-picker
/// overlay. Replaces the old flow of a full-screen activity selector
/// pushing a separate face screen — there is no normal navigation away from
/// this screen anymore.
///
/// Owns the [VoiceSessionController] lifecycle directly: creates one for
/// the default (first enabled, by sort_order) activity on startup, and
/// replaces it wholesale whenever the child picks a different activity from
/// the settings modal.
class RobotHomeScreen extends StatefulWidget {
  const RobotHomeScreen({
    super.key,
    required this.repository,
    required this.resolveAuth,
  });

  final ActivityRepository repository;
  final Future<GeminiAuth> Function() resolveAuth;

  @override
  State<RobotHomeScreen> createState() => _RobotHomeScreenState();
}

class _RobotHomeScreenState extends State<RobotHomeScreen> {
  List<Activity> _activities = const [];
  Activity? _currentActivity;
  VoiceSessionController? _controller;
  bool _settingsOpen = false;
  bool _showHint = false;
  bool _initialLoadDone = false;
  Timer? _hintTimer;
  bool _debugVisible = false;
  late final ValueNotifier<double> _idleAmplitude;

  bool get _isFreeConversation =>
      _currentActivity?.interactionMode == InteractionMode.freeConversation;

  bool get _isSessionRunning {
    final s = _controller?.state;
    return s != null &&
        s != ConversationState.disconnected &&
        s != ConversationState.error;
  }

  @override
  void initState() {
    super.initState();
    _idleAmplitude = ValueNotifier(0);
    WakelockPlus.enable();
    _loadInitialActivity();
  }

  Future<void> _loadInitialActivity() async {
    try {
      final activities = await widget.repository.getEnabledActivities();
      if (!mounted) return;
      setState(() {
        _activities = activities;
        _initialLoadDone = true;
      });
      if (activities.isNotEmpty) {
        // Already sorted by sort_order then title, per
        // ActivityRepository.getEnabledActivities' contract.
        await _startActivity(activities.first);
      }
    } catch (e, stackTrace) {
      debugPrint(
        '[RobotHome] failed to load initial activities: $e\n$stackTrace',
      );
      if (!mounted) return;
      setState(() => _initialLoadDone = true);
      // No activity to start — the face falls back to offline; the
      // settings modal's refresh button is the recovery path.
    }
  }

  Future<void> _startActivity(Activity activity) async {
    final oldController = _controller;
    final controller = VoiceSessionController(
      resolveAuth: widget.resolveAuth,
      systemInstruction: composeSystemInstruction(activity),
      interactionMode: activity.interactionMode,
    )..addListener(_onControllerChanged);

    setState(() {
      _currentActivity = activity;
      _controller = controller;
    });

    // Dispose the previous session (mic/playback/socket) only after the new
    // one is already wired up, so there's no gap where neither is present.
    oldController
      ?..removeListener(_onControllerChanged)
      ..dispose();

    if (activity.interactionMode == InteractionMode.freeConversation) {
      controller.startFreeConversation();
    }
    _showOnboardingHint();
  }

  void _onControllerChanged() => setState(() {});

  void _showOnboardingHint() {
    _hintTimer?.cancel();
    setState(() => _showHint = true);
    _hintTimer = Timer(const Duration(seconds: 3), () {
      if (!mounted) return;
      setState(() => _showHint = false);
    });
  }

  /// Keeps this screen's own activity cache — the thing that seeds
  /// [SettingsModal.initialActivities] the *next* time the modal opens —
  /// in sync with every successful refresh the modal performs. Previously
  /// nothing did this: the modal refreshed only its own local state, so a
  /// newly-created Supabase activity would show up once and then vanish
  /// again the moment the modal was closed and reopened, reseeded from
  /// this screen's stale startup-time snapshot.
  ///
  /// Also handles the *currently selected* activity itself having changed
  /// on the server (edited from Web — e.g. interaction_mode, prompt,
  /// participants) while its id stayed the same: [ActivityRepository]
  /// returns a brand-new [Activity] instance every call, but the previous
  /// version of this method only checked whether an activity with that id
  /// *still existed* — since it did, `_currentActivity` (and the
  /// [VoiceSessionController] built from it, still holding the old
  /// `interactionMode`/composed prompt) were silently kept, even though
  /// the object itself was now stale. That's the exact bug: the UI showed
  /// the refreshed mode, but the live session kept behaving like the old
  /// one. See [_reconfigureCurrentActivity].
  void _handleActivitiesRefreshed(List<Activity> activities) {
    setState(() => _activities = activities);
    final current = _currentActivity;
    if (current == null) return;

    Activity? refreshedCurrent;
    for (final activity in activities) {
      if (activity.id == current.id) {
        refreshedCurrent = activity;
        break;
      }
    }

    if (refreshedCurrent == null) {
      // No longer enabled/deleted remotely — genuinely a different
      // activity now, not a reconfigure; same fallback rule as startup.
      if (activities.isNotEmpty) _startActivity(activities.first);
      return;
    }

    // Activity.== compares every runtime-relevant field (see its doc) —
    // id equality alone isn't enough to know nothing changed.
    if (refreshedCurrent != current) {
      _reconfigureCurrentActivity(refreshedCurrent);
    }
  }

  /// Same id as [_currentActivity], but the activity itself changed on the
  /// server — swaps in a fresh [VoiceSessionController] built from the
  /// *refreshed* prompt/participants/interactionMode, replacing the stale
  /// one. Deliberately never auto-starts the new controller here (even for
  /// free conversation): this only ever runs while the settings modal is
  /// open (triggered by its refresh action), so starting immediately would
  /// make the robot start listening while the modal is still open.
  /// [_openSettings] decides whether to start the new controller once the
  /// modal actually closes, based on whether the session was running
  /// *before* the modal opened — respecting the global Start/Stop state.
  void _reconfigureCurrentActivity(Activity refreshed) {
    final oldController = _controller;
    final controller = VoiceSessionController(
      resolveAuth: widget.resolveAuth,
      systemInstruction: composeSystemInstruction(refreshed),
      interactionMode: refreshed.interactionMode,
    )..addListener(_onControllerChanged);

    setState(() {
      _currentActivity = refreshed;
      _controller = controller;
    });

    oldController
      ?..removeListener(_onControllerChanged)
      ..dispose();
  }

  Future<void> _openSettings() async {
    final controller = _controller;
    final wasRunning = _isSessionRunning;
    await controller?.suspendForOverlay();
    if (!mounted) return;
    setState(() => _settingsOpen = true);

    final selected = await showDialog<Activity>(
      context: context,
      builder: (_) => SettingsModal(
        repository: widget.repository,
        initialActivities: _activities,
        currentActivityId: _currentActivity?.id,
        onActivitiesRefreshed: _handleActivitiesRefreshed,
      ),
    );

    if (!mounted) return;
    setState(() => _settingsOpen = false);

    if (selected != null && selected.id != _currentActivity?.id) {
      await _startActivity(selected);
      return;
    }

    if (identical(_controller, controller)) {
      // Nothing changed underneath us — just resume what we suspended.
      await controller?.resumeFromOverlay();
      return;
    }

    // The controller changed without an explicit selection: a refresh
    // inside the modal reconfigured the *current* activity in place (see
    // _reconfigureCurrentActivity) with a fresh, still-idle controller.
    // Only start it if the session was actually running before the modal
    // opened — a global Stop must stay stopped; the next manual Start
    // will use the new mode automatically, since _controller/
    // _currentActivity already reflect it.
    if (wasRunning) {
      final current = _controller;
      if (current == null) return;
      if (current.interactionMode == InteractionMode.freeConversation) {
        await current.startFreeConversation();
      } else {
        await current.startSession();
      }
    }
  }

  bool get _canRetry {
    final s = _controller?.state;
    return s == null ||
        s == ConversationState.disconnected ||
        s == ConversationState.error;
  }

  void _onMouthTapDown() {
    if (_settingsOpen || _currentActivity == null) return;
    if (_isFreeConversation) return;
    final controller = _controller!;
    if (_canRetry) {
      controller.onPressStart();
    } else if (controller.state == ConversationState.ready) {
      controller.onPressStart();
    }
  }

  void _onMouthTapUp() {
    if (_settingsOpen || _currentActivity == null || _isFreeConversation) {
      return;
    }
    _controller!.onPressEnd();
  }

  Future<void> _toggleSession() async {
    final controller = _controller;
    if (controller == null) return;
    if (_isSessionRunning) {
      await controller.stopSession();
    } else {
      await controller.startSession();
    }
  }

  String? get _hintText {
    if (_currentActivity == null) return null;
    if (_isFreeConversation) return 'Te ascult';
    return 'Ține apăsat pe gură pentru a vorbi';
  }

  @override
  void dispose() {
    _hintTimer?.cancel();
    _idleAmplitude.dispose();
    _controller
      ?..removeListener(_onControllerChanged)
      ..dispose();
    WakelockPlus.disable();
    super.dispose();
  }

  RobotExpression get _expression {
    final controller = _controller;
    if (controller != null) {
      return mapConversationStateToExpression(controller.state);
    }
    // No session yet: neutral while we're still finding out what to start
    // (Part J — render the face immediately, don't wait on the network),
    // offline once loading finished and there was nothing to start.
    return _initialLoadDone ? RobotExpression.offline : RobotExpression.neutral;
  }

  @override
  Widget build(BuildContext context) {
    final amplitude = _controller?.speakingAmplitude ?? _idleAmplitude;

    return Scaffold(
      backgroundColor: TthColors.faceBackground,
      body: SafeArea(
        child: Stack(
          children: [
            Positioned.fill(
              child: RobotFace(expression: _expression, amplitude: amplitude),
            ),
            if (_currentActivity != null &&
                _currentActivity!.interactionMode == InteractionMode.pushToTalk)
              Align(
                alignment: Alignment.bottomCenter,
                child: FractionallySizedBox(
                  widthFactor: 0.6,
                  heightFactor: 0.45,
                  child: GestureDetector(
                    key: const ValueKey('ptt-mouth-gesture'),
                    behavior: HitTestBehavior.opaque,
                    onTapDown: _settingsOpen ? null : (_) => _onMouthTapDown(),
                    onTapUp: (_) => _onMouthTapUp(),
                    onTapCancel: _onMouthTapUp,
                  ),
                ),
              ),
            Positioned(
              bottom: 20,
              left: 0,
              right: 0,
              child: IgnorePointer(
                child: AnimatedOpacity(
                  opacity: _showHint && !_settingsOpen ? 1 : 0,
                  duration: const Duration(milliseconds: 400),
                  child: Center(
                    child: Text(
                      _hintText ?? '',
                      style: const TextStyle(
                        color: Colors.white70,
                        fontSize: 15,
                      ),
                    ),
                  ),
                ),
              ),
            ),
            Positioned(
              top: 8,
              left: 8,
              child: _StartStopButton(
                running: _isSessionRunning,
                enabled: _controller != null,
                onTap: _toggleSession,
              ),
            ),
            Positioned(
              top: 8,
              right: 8,
              child: _SettingsGearButton(
                onTap: _openSettings,
                // Debug-only affordance: never shown to a child, only
                // reachable by a developer who knows to long-press the
                // gear — see Phase 5 remediation's "no permanent debug
                // panel" requirement.
                onLongPress: kDebugMode
                    ? () => setState(() => _debugVisible = !_debugVisible)
                    : null,
              ),
            ),
            if (kDebugMode && _debugVisible && _controller != null)
              _DebugOverlay(
                controller: _controller!,
                activity: _currentActivity!,
              ),
          ],
        ),
      ),
    );
  }
}

class _SettingsGearButton extends StatelessWidget {
  const _SettingsGearButton({required this.onTap, this.onLongPress});

  final VoidCallback onTap;
  final VoidCallback? onLongPress;

  @override
  Widget build(BuildContext context) {
    return Container(
      decoration: const BoxDecoration(
        color: _kButtonFill,
        shape: BoxShape.circle,
        border: Border.fromBorderSide(_kButtonBorder),
      ),
      child: Material(
        color: Colors.transparent,
        shape: const CircleBorder(),
        child: InkWell(
          customBorder: const CircleBorder(),
          onTap: onTap,
          onLongPress: onLongPress,
          child: const SizedBox(
            width: 48,
            height: 48,
            child: Icon(Icons.settings, color: Colors.white70, size: 22),
          ),
        ),
      ),
    );
  }
}

/// The other of the two permanent controls (Phase 5 remediation): starts or
/// fully stops the active session — see [VoiceSessionController.startSession]
/// / [VoiceSessionController.stopSession]. Dimmed and inert until an
/// activity has actually loaded.
class _StartStopButton extends StatelessWidget {
  const _StartStopButton({
    required this.running,
    required this.enabled,
    required this.onTap,
  });

  final bool running;
  final bool enabled;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    return Container(
      decoration: BoxDecoration(
        color: enabled ? _kButtonFill : const Color(0x0DFFFFFF),
        shape: BoxShape.circle,
        border: Border.fromBorderSide(_kButtonBorder),
      ),
      child: Material(
        color: Colors.transparent,
        shape: const CircleBorder(),
        child: InkWell(
          customBorder: const CircleBorder(),
          onTap: enabled ? onTap : null,
          child: SizedBox(
            width: 48,
            height: 48,
            child: Icon(
              running ? Icons.stop_rounded : Icons.play_arrow_rounded,
              // A touch of cyan on the play glyph, matching the reference's
              // accent-tinted start icon — stop stays neutral white.
              color: !enabled
                  ? Colors.white24
                  : running
                  ? Colors.white70
                  : TthColors.faceCyan,
              size: 24,
            ),
          ),
        ),
      ),
    );
  }
}

class _DebugOverlay extends StatelessWidget {
  const _DebugOverlay({required this.controller, required this.activity});

  final VoiceSessionController controller;
  final Activity activity;

  static const _style = TextStyle(
    color: Colors.greenAccent,
    fontSize: 12,
    fontFamily: 'monospace',
  );

  @override
  Widget build(BuildContext context) {
    return Positioned(
      top: 40,
      left: 8,
      child: Container(
        padding: const EdgeInsets.all(10),
        decoration: BoxDecoration(
          color: Colors.black.withValues(alpha: 0.75),
          borderRadius: BorderRadius.circular(8),
        ),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          mainAxisSize: MainAxisSize.min,
          children: [
            Text('Activitate: ${activity.title}', style: _style),
            Text('Stare: ${controller.state.name}', style: _style),
            Text('Mod: ${controller.interactionMode.name}', style: _style),
            Text('Prompt: ${controller.systemInstruction}', style: _style),
            Text('Suspendat: ${controller.isSuspended}', style: _style),
            Text(
              'Microfon: ${controller.isMicrophoneActive ? 'active' : 'inactive'}',
              style: _style,
            ),
            Text('Eroare: ${controller.errorMessage ?? '-'}', style: _style),
          ],
        ),
      ),
    );
  }
}
