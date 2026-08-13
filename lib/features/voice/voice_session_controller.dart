import 'dart:async';

import 'package:flutter/foundation.dart';

import '../activities/activity.dart';
import 'gemini_auth.dart';
import 'gemini_live_service.dart';
import 'microphone_service.dart';
import 'pcm_audio_player.dart';
import 'pcm_levels.dart';

enum ConversationState {
  disconnected,
  connecting,
  ready,
  listening,
  waiting,
  speaking,
  error,
}

/// Orchestrates the push-to-talk voice spike: owns the conversation state
/// machine and wires [GeminiLiveService], [MicrophoneService] and
/// [PcmAudioPlayer] together. Contains no UI code.
class VoiceSessionController extends ChangeNotifier {
  VoiceSessionController({
    required Future<GeminiAuth> Function() resolveAuth,
    required String systemInstruction,
    required InteractionMode interactionMode,
    GeminiLiveService? liveService,
    MicrophoneService? microphoneService,
    PcmAudioPlayer? audioPlayer,
  }) : _resolveAuth = resolveAuth,
       _systemInstruction = systemInstruction,
       _interactionMode = interactionMode,
       _live = liveService ?? GeminiLiveService(),
       _mic = microphoneService ?? MicrophoneService(),
       _player = audioPlayer ?? PcmAudioPlayer();

  /// Resolves which [GeminiAuth] mode to use, called once per connect —
  /// see `resolveGeminiAuth` for the ephemeral-token-first, dev-key-fallback
  /// policy normally passed here.
  final Future<GeminiAuth> Function() _resolveAuth;
  final String _systemInstruction;
  final InteractionMode _interactionMode;
  final GeminiLiveService _live;
  final MicrophoneService _mic;
  final PcmAudioPlayer _player;

  ConversationState _state = ConversationState.disconnected;
  ConversationState get state => _state;

  String? _errorMessage;
  String? get errorMessage => _errorMessage;

  /// Normalized (0..1) speaking loudness for driving a mouth-open
  /// animation — see [PcmAudioPlayer.amplitude] for how it's derived and
  /// its known latency limitation.
  ValueListenable<double> get speakingAmplitude => _player.amplitude;

  // --- Debug-overlay-only state; never read for control flow. ---
  InteractionMode get interactionMode => _interactionMode;
  bool get isMicrophoneActive => _microphoneActive;
  DateTime? get lastInterruptionAt => _lastInterruptionAt;

  /// The exact system instruction this session was built with — exposed
  /// so callers (and tests) can confirm a controller was actually built
  /// from a given [Activity]'s *current* composed prompt, rather than a
  /// stale one from before a Web-side edit + refresh.
  String get systemInstruction => _systemInstruction;

  StreamSubscription<GeminiLiveEvent>? _liveSub;
  StreamSubscription<Uint8List>? _micSub;
  Completer<void>? _setupCompleter;
  bool _pressHeld = false;
  bool _microphoneActive = false;
  DateTime? _lastInterruptionAt;

  /// True while the settings overlay has paused this session — see
  /// [suspendForOverlay]. Mic input and inbound audio playback are both
  /// ignored while suspended; the underlying Gemini Live connection stays
  /// open so [resumeFromOverlay] doesn't have to reconnect.
  bool _suspended = false;
  bool get isSuspended => _suspended;

  /// How many of the current turn's audio chunks have been logged via
  /// `[AudioDebug]` — capped at [_maxAudioDebugChunksPerTurn] so a long
  /// response doesn't spam the log; reset at the start of every turn.
  int _turnAudioDebugChunkCount = 0;
  static const int _maxAudioDebugChunksPerTurn = 3;

  /// Where the state machine settles between turns: back to `ready`
  /// (waiting for a press) for push-to-talk, or `listening` for free
  /// conversation — the microphone never stops there, so "waiting for the
  /// child" *is* the resting state.
  ConversationState get _restingState =>
      _interactionMode == InteractionMode.freeConversation
      ? ConversationState.listening
      : ConversationState.ready;

  void _setState(ConversationState next, {String? error}) {
    _state = next;
    _errorMessage = error;
    notifyListeners();
  }

  /// Call when the user presses and holds the push-to-talk button.
  /// [InteractionMode.pushToTalk] only — see [startFreeConversation] for
  /// the other mode. Unaffected by the free-conversation work below.
  Future<void> onPressStart() async {
    assert(_interactionMode == InteractionMode.pushToTalk);
    _pressHeld = true;

    if (_state == ConversationState.disconnected ||
        _state == ConversationState.error) {
      _setState(ConversationState.connecting);
      try {
        await _connect();
      } catch (e) {
        _setState(ConversationState.error, error: 'Conectare eșuată: $e');
        _pressHeld = false;
        return;
      }
      if (!_pressHeld) {
        // User released while we were still connecting; stay ready for the
        // next press instead of starting to listen now.
        _setState(ConversationState.ready);
        return;
      }
    }

    if (_state != ConversationState.ready) {
      // Busy with a previous turn (listening/waiting/speaking) — ignore.
      return;
    }

    final granted = await _mic.hasPermission();
    if (!granted) {
      _pressHeld = false;
      _setState(
        ConversationState.error,
        error:
            'Permisiunea de microfon a fost refuzată. Activeaz-o din '
            'setările telefonului pentru a continua.',
      );
      return;
    }

    _setState(ConversationState.listening);
    _live.sendActivityStart();
    final micStream = await _mic.start();
    _microphoneActive = true;
    _micSub = micStream.listen(_live.sendAudioChunk);
  }

  /// Call when the user releases the push-to-talk button.
  Future<void> onPressEnd() async {
    assert(_interactionMode == InteractionMode.pushToTalk);
    _pressHeld = false;
    if (_state != ConversationState.listening) return;

    await _stopMicrophone();
    _live.sendActivityEnd();
    _setState(ConversationState.waiting);
  }

  /// Starts (or retries) a [InteractionMode.freeConversation] session: no
  /// press is involved. Connects if needed; once `setupComplete` arrives,
  /// [_handleLiveEvent] automatically begins continuous microphone
  /// streaming via [_beginContinuousListening] — see that method for why
  /// the trigger lives there instead of here (it must also fire after an
  /// automatic reconnect, not just the first call).
  ///
  /// A no-op unless currently disconnected/in error, so it's safe to call
  /// both to start the session initially and, from the UI, as a
  /// tap-to-retry action after a connection failure.
  Future<void> startFreeConversation() async {
    assert(_interactionMode == InteractionMode.freeConversation);
    if (_state != ConversationState.disconnected &&
        _state != ConversationState.error) {
      return;
    }
    _setState(ConversationState.connecting);
    try {
      await _connect();
    } catch (e) {
      _setState(ConversationState.error, error: 'Conectare eșuată: $e');
    }
  }

  /// Begins continuous microphone streaming for free conversation. Called
  /// only from the [GeminiSetupComplete] branch of [_handleLiveEvent], so
  /// it naturally re-runs after every successful (re)connect without a
  /// second call site to keep in sync — and the [_microphoneActive] guard
  /// below means it's a no-op if, somehow, it's already running.
  Future<void> _beginContinuousListening() async {
    if (_microphoneActive) return;

    final granted = await _mic.hasPermission();
    if (!granted) {
      _setState(
        ConversationState.error,
        error:
            'Permisiunea de microfon a fost refuzată. Activeaz-o din '
            'setările telefonului pentru a continua.',
      );
      return;
    }

    final micStream = await _mic.start();
    _microphoneActive = true;
    _micSub = micStream.listen(_live.sendAudioChunk);
    _setState(ConversationState.listening);
  }

  /// Shared by push-to-talk's per-turn stop ([onPressEnd]) and free
  /// conversation's session-teardown stop (unexpected disconnect,
  /// [dispose]). No-ops if the microphone isn't actually active, so it's
  /// always safe to call defensively.
  Future<void> _stopMicrophone() async {
    if (!_microphoneActive) return;
    _microphoneActive = false;
    await _micSub?.cancel();
    _micSub = null;
    await _mic.stop();
  }

  /// Pauses this session while the settings overlay is open: stops
  /// microphone capture (so the mic never listens to room conversation
  /// behind the modal) and stops any in-progress playback, without closing
  /// the underlying Gemini Live connection. Idempotent — safe to call even
  /// if already suspended or not yet connected.
  Future<void> suspendForOverlay() async {
    if (_suspended) return;
    _suspended = true;
    await _stopMicrophone();
    _player.stopTurn();
    if (_state == ConversationState.speaking ||
        _state == ConversationState.listening ||
        _state == ConversationState.waiting) {
      _setState(ConversationState.ready);
    }
  }

  /// Reverses [suspendForOverlay]. For free conversation this resumes
  /// continuous listening (via the same idempotent
  /// [_beginContinuousListening] used after a reconnect, so it can never
  /// create a second microphone stream); for push-to-talk it simply makes
  /// the session touchable again — a press starts listening, as always. A
  /// no-op if not currently suspended, or if the connection dropped while
  /// suspended (the existing disconnected/error retry path handles that).
  Future<void> resumeFromOverlay() async {
    if (!_suspended) return;
    _suspended = false;
    if (_state == ConversationState.disconnected ||
        _state == ConversationState.error) {
      return;
    }
    if (_interactionMode == InteractionMode.freeConversation) {
      await _beginContinuousListening();
    }
  }

  /// Manual "Stop" control (the robot face's start/stop button): a full
  /// teardown, not a pause — stops the microphone, clears/stops any queued
  /// or playing audio, and disconnects the Gemini Live socket, so free
  /// conversation truly cannot react to ambient room noise while stopped.
  /// A no-op if already disconnected/error. Safe to call even mid-turn.
  Future<void> stopSession() async {
    if (_state == ConversationState.disconnected ||
        _state == ConversationState.error) {
      return;
    }
    await _stopMicrophone();
    _player.stopTurn();
    await _live.disconnect();
    // The socket closing also emits a GeminiClosed event asynchronously
    // (see GeminiLiveService._onDone), which lands in _handleLiveEvent and
    // performs the same teardown again — harmless (both stopMicrophone and
    // stopTurn are idempotent) — but setting state here immediately avoids
    // a visible delay before the face reflects the stop.
    _setState(ConversationState.disconnected);
  }

  /// Manual "Start" control: reconnects. For free conversation this is the
  /// same entry point [startFreeConversation] uses (including as a
  /// tap-to-retry action), so continuous listening resumes normally; for
  /// push-to-talk it connects proactively and settles on `ready` rather
  /// than waiting for a first press to pay the connect latency. A no-op
  /// unless currently disconnected/in error.
  Future<void> startSession() async {
    if (_state != ConversationState.disconnected &&
        _state != ConversationState.error) {
      return;
    }
    if (_interactionMode == InteractionMode.freeConversation) {
      await startFreeConversation();
      return;
    }
    _setState(ConversationState.connecting);
    try {
      await _connect();
      _setState(ConversationState.ready);
    } catch (e) {
      _setState(ConversationState.error, error: 'Conectare eșuată: $e');
    }
  }

  Future<void> _connect() async {
    // Cancel any subscription left over from a previous connection before
    // creating a new one — otherwise, after a reconnect, both the stale
    // and the fresh listener would be attached to the same persistent
    // GeminiLiveService.events broadcast stream, and every server event
    // would be handled twice (double state transitions, audio fed to the
    // player twice, etc.). This one call is what makes reconnects safe for
    // both interaction modes.
    await _liveSub?.cancel();
    _setupCompleter = Completer<void>();
    _liveSub = _live.events.listen(_handleLiveEvent);
    final auth = await _resolveAuth();
    await _live.connect(
      auth: auth,
      systemInstruction: _systemInstruction,
      interactionMode: _interactionMode,
    );
    if (_interactionMode == InteractionMode.freeConversation) {
      debugPrint('[VAD] automatic mode enabled');
    }
    await _player.init();
    await _setupCompleter!.future.timeout(
      const Duration(seconds: 15),
      onTimeout: () => throw TimeoutException('setupComplete never arrived'),
    );
  }

  void _handleLiveEvent(GeminiLiveEvent event) {
    switch (event) {
      case GeminiSetupComplete():
        if (_setupCompleter?.isCompleted == false) {
          _setupCompleter!.complete();
        }
        if (_interactionMode == InteractionMode.freeConversation) {
          _beginContinuousListening();
        } else {
          _setState(ConversationState.ready);
        }

      case GeminiAudioChunk(:final pcm16):
        // Dropped rather than played while the settings overlay is open —
        // see suspendForOverlay. The Live connection stays open, so a
        // turn already in flight when the overlay opened can still
        // deliver trailing chunks here; they must not restart playback.
        if (_suspended) return;
        if (_state == ConversationState.waiting ||
            _state == ConversationState.listening) {
          _player.startTurn();
          _setState(ConversationState.speaking);
          _turnAudioDebugChunkCount = 0;
          debugPrint('[VAD] model turn started');
        }
        if (_turnAudioDebugChunkCount < _maxAudioDebugChunksPerTurn) {
          // DEV-ONLY: compares Gemini's actual output signal level between
          // interaction modes — see the Phase 4B stabilization report. If
          // this reads roughly the same in both modes (expected), the
          // low-volume bug is in routing/playback, not the PCM itself.
          debugPrint(
            '[AudioDebug] mode=${_interactionMode.dbValue} '
            '${computePcmLevels(pcm16)}',
          );
          _turnAudioDebugChunkCount++;
        }
        _player.feed(pcm16);

      case GeminiTurnComplete():
        _player.endTurn();
        debugPrint('[VAD] model turn completed');
        if (_suspended) return;
        if (_state == ConversationState.speaking ||
            _state == ConversationState.waiting) {
          _setState(_restingState);
        }

      case GeminiInterrupted():
        // Stops playback and clears any buffered response audio for the
        // interrupted turn immediately (see PcmAudioPlayer.stopTurn —
        // disposes the current buffer stream and zeroes the mouth
        // amplitude synchronously), and never touches the microphone, so
        // continuous free-conversation streaming — the mechanism that
        // let the child's speech trigger this interruption in the first
        // place — is completely unaffected.
        debugPrint('[VAD] interruption received');
        _player.stopTurn();
        _lastInterruptionAt = DateTime.now();
        if (_state == ConversationState.speaking) {
          _setState(_restingState);
        }

      case GeminiServerError(:final message):
        // Unblock a pending connect() instead of leaving it hanging until
        // its timeout if the error arrives before setupComplete.
        if (_setupCompleter?.isCompleted == false) {
          _setupCompleter!.complete();
        }
        _setState(ConversationState.error, error: message);

      case GeminiClosed(:final reason):
        if (_setupCompleter?.isCompleted == false) {
          _setupCompleter!.complete();
        }
        _stopMicrophone();
        _player.stopTurn();
        if (_state != ConversationState.error) {
          _setState(
            ConversationState.disconnected,
            error: reason == null || reason.isEmpty
                ? null
                : 'Conexiune închisă: $reason',
          );
        }
    }
  }

  @override
  void dispose() {
    _stopMicrophone();
    _liveSub?.cancel();
    _mic.dispose();
    _live.dispose();
    _player.dispose();
    super.dispose();
  }
}
