import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:wakelock_plus/wakelock_plus.dart';

import '../activities/activity.dart';
import '../activities/prompt_composer.dart';
import '../voice/gemini_auth.dart';
import '../voice/voice_session_controller.dart';
import 'robot_expression.dart';
import 'robot_face.dart';

/// The robot-face screen for one activity session. Behavior forks on
/// [Activity.interactionMode] — the source of truth per this task, never
/// the activity's title/type:
///
/// * [InteractionMode.pushToTalk]: connects lazily on the first press
///   (unchanged from the original voice spike).
/// * [InteractionMode.freeConversation]: connects automatically in
///   [initState] and, once set up, streams the microphone continuously —
///   see [VoiceSessionController.startFreeConversation]. Touch does
///   nothing except retry after a connection error.
///
/// Either way this shows [RobotFace] reacting to [ConversationState] and
/// keeps all WebSocket/technical detail behind a hidden developer overlay
/// so children only ever see the face.
class RobotFaceScreen extends StatefulWidget {
  const RobotFaceScreen({
    super.key,
    required this.activity,
    required this.resolveAuth,
  });

  final Activity activity;

  /// How to authenticate the Gemini Live connection for this session — see
  /// `resolveGeminiAuth` for the ephemeral-token-first, dev-key-fallback
  /// policy `main.dart` normally passes here.
  final Future<GeminiAuth> Function() resolveAuth;

  @override
  State<RobotFaceScreen> createState() => _RobotFaceScreenState();
}

class _RobotFaceScreenState extends State<RobotFaceScreen> {
  late final VoiceSessionController _controller;
  bool _debugVisible = false;

  bool get _isFreeConversation =>
      widget.activity.interactionMode == InteractionMode.freeConversation;

  @override
  void initState() {
    super.initState();
    _controller = VoiceSessionController(
      resolveAuth: widget.resolveAuth,
      systemInstruction: composeSystemInstruction(widget.activity),
      interactionMode: widget.activity.interactionMode,
    )..addListener(_onControllerChanged);

    SystemChrome.setPreferredOrientations([
      DeviceOrientation.landscapeLeft,
      DeviceOrientation.landscapeRight,
    ]);
    WakelockPlus.enable();

    if (_isFreeConversation) {
      _controller.startFreeConversation();
    }
  }

  void _onControllerChanged() => setState(() {});

  @override
  void dispose() {
    _controller
      ..removeListener(_onControllerChanged)
      ..dispose();
    WakelockPlus.disable();
    super.dispose();
  }

  bool get _canRetry {
    final s = _controller.state;
    return s == ConversationState.disconnected || s == ConversationState.error;
  }

  bool get _touchEnabled {
    if (_isFreeConversation) {
      // No press-to-talk in this mode — touch only ever retries a dropped
      // connection; otherwise it does nothing, per this task's explicit
      // "do not accidentally send activityStart/activityEnd on touch".
      return _canRetry;
    }
    return _canRetry || _controller.state == ConversationState.ready;
  }

  String get _instructionLabel {
    if (_isFreeConversation) {
      switch (_controller.state) {
        case ConversationState.connecting:
          return 'Se conectează...';
        case ConversationState.ready:
        case ConversationState.listening:
          return 'Te ascult';
        case ConversationState.waiting:
          return 'Mă gândesc...';
        case ConversationState.speaking:
          return '';
        case ConversationState.disconnected:
        case ConversationState.error:
          return 'Apasă pentru a reîncerca';
      }
    }

    switch (_controller.state) {
      case ConversationState.listening:
        return 'Te ascult...';
      case ConversationState.waiting:
        return 'Mă gândesc...';
      case ConversationState.speaking:
        return '';
      case ConversationState.connecting:
        return 'Se conectează...';
      case ConversationState.error:
        return 'Apasă pentru a reîncerca';
      case ConversationState.disconnected:
      case ConversationState.ready:
        return 'Ține apăsat și vorbește';
    }
  }

  void _onTapDown() {
    if (_isFreeConversation) {
      if (_canRetry) _controller.startFreeConversation();
      return;
    }
    _controller.onPressStart();
  }

  void _onTapUp() {
    if (_isFreeConversation) return;
    _controller.onPressEnd();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: Colors.black,
      body: SafeArea(
        child: Stack(
          children: [
            Center(
              child: GestureDetector(
                behavior: HitTestBehavior.opaque,
                onTapDown: _touchEnabled ? (_) => _onTapDown() : null,
                onTapUp: (_) => _onTapUp(),
                onTapCancel: _onTapUp,
                child: SizedBox(
                  width: 360,
                  height: 320,
                  child: RobotFace(
                    expression: mapConversationStateToExpression(
                      _controller.state,
                    ),
                    amplitude: _controller.speakingAmplitude,
                  ),
                ),
              ),
            ),
            Positioned(
              bottom: 24,
              left: 0,
              right: 0,
              child: IgnorePointer(
                child: Center(
                  child: Text(
                    _instructionLabel,
                    style: const TextStyle(color: Colors.white70, fontSize: 16),
                  ),
                ),
              ),
            ),
            Positioned(
              top: 8,
              left: 8,
              child: _DevGesture(
                onTap: () => setState(() => _debugVisible = !_debugVisible),
              ),
            ),
            if (_debugVisible)
              _DebugOverlay(controller: _controller, activity: widget.activity),
          ],
        ),
      ),
    );
  }
}

/// Small, low-opacity developer affordance — not meant to be noticed by
/// children, just tappable by whoever is testing the build.
class _DevGesture extends StatelessWidget {
  const _DevGesture({required this.onTap});

  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    return GestureDetector(
      onTap: onTap,
      child: const Padding(
        padding: EdgeInsets.all(12),
        child: Opacity(
          opacity: 0.25,
          child: Icon(Icons.bug_report, color: Colors.white, size: 20),
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
    final lastInterruption = controller.lastInterruptionAt;
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
            Text(
              'VAD: ${controller.interactionMode == InteractionMode.freeConversation ? 'automatic' : 'manual'}',
              style: _style,
            ),
            Text(
              'Microfon: ${controller.isMicrophoneActive ? 'active' : 'inactive'}',
              style: _style,
            ),
            Text(
              'Playback: ${controller.state == ConversationState.speaking ? 'playing' : 'stopped'}',
              style: _style,
            ),
            Text(
              'Ultima întrerupere: '
              '${lastInterruption == null ? '-' : lastInterruption.toIso8601String()}',
              style: _style,
            ),
            Text('Eroare: ${controller.errorMessage ?? '-'}', style: _style),
            const SizedBox(height: 6),
            TextButton(
              style: TextButton.styleFrom(padding: EdgeInsets.zero),
              onPressed: () => Navigator.of(context).pop(),
              child: const Text(
                'Înapoi la activități',
                style: TextStyle(color: Colors.cyanAccent, fontSize: 12),
              ),
            ),
          ],
        ),
      ),
    );
  }
}
