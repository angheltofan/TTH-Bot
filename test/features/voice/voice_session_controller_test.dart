import 'dart:async';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:tth_bot/features/activities/activity.dart';
import 'package:tth_bot/features/voice/gemini_auth.dart';
import 'package:tth_bot/features/voice/gemini_live_service.dart';
import 'package:tth_bot/features/voice/microphone_service.dart';
import 'package:tth_bot/features/voice/pcm_audio_player.dart';
import 'package:tth_bot/features/voice/voice_session_controller.dart';

/// Test double that skips the real WebSocket and lets the test drive
/// server events via [emitEventForTest].
class _FakeGeminiLiveService extends GeminiLiveService {
  int connectCallCount = 0;
  bool get connectCalled => connectCallCount > 0;
  InteractionMode? lastInteractionMode;
  bool activityStartSent = false;
  bool activityEndSent = false;
  final List<Uint8List> sentChunks = [];

  @override
  Future<void> connect({
    required GeminiAuth auth,
    required String systemInstruction,
    required InteractionMode interactionMode,
  }) async {
    connectCallCount++;
    lastInteractionMode = interactionMode;
  }

  @override
  void sendActivityStart() => activityStartSent = true;

  @override
  void sendActivityEnd() => activityEndSent = true;

  @override
  void sendAudioChunk(Uint8List pcm16) => sentChunks.add(pcm16);

  @override
  Future<void> disconnect() async {}

  @override
  Future<void> dispose() async {}
}

class _FakeMicrophoneService extends MicrophoneService {
  bool permissionGranted = true;
  int startCallCount = 0;
  bool get startCalled => startCallCount > 0;
  bool stopCalled = false;
  final StreamController<Uint8List> chunkController =
      StreamController<Uint8List>.broadcast();

  @override
  Future<bool> hasPermission() async => permissionGranted;

  @override
  Future<Stream<Uint8List>> start() async {
    startCallCount++;
    return chunkController.stream;
  }

  @override
  Future<void> stop() async {
    stopCalled = true;
  }

  @override
  Future<void> dispose() async {}
}

class _FakePcmAudioPlayer extends PcmAudioPlayer {
  bool initCalled = false;
  int startTurnCalls = 0;
  int endTurnCalls = 0;
  int stopTurnCalls = 0;
  final List<Uint8List> fed = [];

  @override
  Future<void> init() async {
    initCalled = true;
  }

  @override
  void startTurn() => startTurnCalls++;

  @override
  void feed(Uint8List pcm16) => fed.add(pcm16);

  @override
  void endTurn() => endTurnCalls++;

  @override
  void stopTurn() => stopTurnCalls++;

  @override
  Future<void> dispose() async {}
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  group('push-to-talk', () {
    late _FakeGeminiLiveService live;
    late _FakeMicrophoneService mic;
    late _FakePcmAudioPlayer player;
    late VoiceSessionController controller;

    setUp(() {
      live = _FakeGeminiLiveService();
      mic = _FakeMicrophoneService();
      player = _FakePcmAudioPlayer();
      controller = VoiceSessionController(
        resolveAuth: () async => const GeminiApiKeyAuth('test-key'),
        systemInstruction: 'system instruction',
        interactionMode: InteractionMode.pushToTalk,
        liveService: live,
        microphoneService: mic,
        audioPlayer: player,
      );
    });

    test('initial state is disconnected', () {
      expect(controller.state, ConversationState.disconnected);
    });

    test('connect uses manual VAD (pushToTalk)', () async {
      final pressFuture = controller.onPressStart();
      await Future<void>.delayed(Duration.zero);
      live.emitEventForTest(const GeminiSetupComplete());
      await pressFuture;

      expect(live.lastInteractionMode, InteractionMode.pushToTalk);
    });

    test('a failing auth resolver surfaces a clear error and never sends '
        'realtime audio', () async {
      final noAuthController = VoiceSessionController(
        resolveAuth: () async => throw StateError('no Gemini auth available'),
        systemInstruction: 'x',
        interactionMode: InteractionMode.pushToTalk,
        liveService: live,
        microphoneService: mic,
        audioPlayer: player,
      );

      await noAuthController.onPressStart();

      expect(noAuthController.state, ConversationState.error);
      expect(live.connectCalled, isFalse);
    });

    test('press-and-hold connects, waits for setupComplete, then starts '
        'listening', () async {
      final pressFuture = controller.onPressStart();
      await Future<void>.delayed(Duration.zero);
      expect(controller.state, ConversationState.connecting);
      expect(live.connectCalled, isTrue);

      live.emitEventForTest(const GeminiSetupComplete());
      await pressFuture;

      expect(controller.state, ConversationState.listening);
      expect(live.activityStartSent, isTrue);
      expect(mic.startCalled, isTrue);
      expect(controller.isMicrophoneActive, isTrue);
    });

    test(
      'releasing the button sends activityEnd and moves to waiting',
      () async {
        final pressFuture = controller.onPressStart();
        await Future<void>.delayed(Duration.zero);
        live.emitEventForTest(const GeminiSetupComplete());
        await pressFuture;

        await controller.onPressEnd();

        expect(controller.state, ConversationState.waiting);
        expect(mic.stopCalled, isTrue);
        expect(live.activityEndSent, isTrue);
        expect(controller.isMicrophoneActive, isFalse);
      },
    );

    test('first audio chunk starts speaking and feeds the player; '
        'turnComplete returns to ready for the next turn', () async {
      final pressFuture = controller.onPressStart();
      await Future<void>.delayed(Duration.zero);
      live.emitEventForTest(const GeminiSetupComplete());
      await pressFuture;
      await controller.onPressEnd();

      final chunk = Uint8List.fromList([1, 2, 3]);
      live.emitEventForTest(GeminiAudioChunk(chunk));
      await Future<void>.delayed(Duration.zero);

      expect(controller.state, ConversationState.speaking);
      expect(player.startTurnCalls, 1);
      expect(player.fed, [chunk]);

      live.emitEventForTest(const GeminiTurnComplete());
      await Future<void>.delayed(Duration.zero);

      // PTT's resting state is `ready` (waiting for the next press) — not
      // `listening`, since the mic actually stops between turns here.
      expect(controller.state, ConversationState.ready);
      expect(player.endTurnCalls, 1);
    });

    test(
      'a second turn works over the same connection without reconnecting',
      () async {
        final pressFuture = controller.onPressStart();
        await Future<void>.delayed(Duration.zero);
        live.emitEventForTest(const GeminiSetupComplete());
        await pressFuture;
        await controller.onPressEnd();
        live.emitEventForTest(GeminiAudioChunk(Uint8List.fromList([1])));
        live.emitEventForTest(const GeminiTurnComplete());
        await Future<void>.delayed(Duration.zero);
        expect(controller.state, ConversationState.ready);

        await controller.onPressStart();
        await controller.onPressEnd();
        live.emitEventForTest(GeminiAudioChunk(Uint8List.fromList([2])));
        live.emitEventForTest(const GeminiTurnComplete());
        await Future<void>.delayed(Duration.zero);

        expect(controller.state, ConversationState.ready);
        expect(player.startTurnCalls, 2);
        // connect() must only run once across both turns.
        expect(live.connectCallCount, 1);
      },
    );

    test('interrupted event stops playback, resets amplitude, and returns '
        'to ready', () async {
      final pressFuture = controller.onPressStart();
      await Future<void>.delayed(Duration.zero);
      live.emitEventForTest(const GeminiSetupComplete());
      await pressFuture;
      await controller.onPressEnd();
      live.emitEventForTest(GeminiAudioChunk(Uint8List.fromList([1])));
      await Future<void>.delayed(Duration.zero);

      live.emitEventForTest(const GeminiInterrupted());
      await Future<void>.delayed(Duration.zero);

      expect(controller.state, ConversationState.ready);
      expect(player.stopTurnCalls, 1);
      expect(controller.lastInterruptionAt, isNotNull);
    });

    test('denied microphone permission surfaces a clear error and does not '
        'start listening', () async {
      mic.permissionGranted = false;

      final pressFuture = controller.onPressStart();
      await Future<void>.delayed(Duration.zero);
      live.emitEventForTest(const GeminiSetupComplete());
      await pressFuture;

      expect(controller.state, ConversationState.error);
      expect(controller.errorMessage, isNotNull);
      expect(mic.startCalled, isFalse);
    });

    test(
      'a server error event moves the controller to the error state',
      () async {
        final pressFuture = controller.onPressStart();
        await Future<void>.delayed(Duration.zero);
        live.emitEventForTest(const GeminiServerError('boom'));
        await pressFuture;

        expect(controller.state, ConversationState.error);
        expect(controller.errorMessage, 'boom');
      },
    );

    test(
      'stopSession stops the microphone, clears playback and disconnects; '
      'startSession reconnects and settles on ready without a press',
      () async {
        final pressFuture = controller.onPressStart();
        await Future<void>.delayed(Duration.zero);
        live.emitEventForTest(const GeminiSetupComplete());
        await pressFuture;
        await controller.onPressEnd();
        live.emitEventForTest(GeminiAudioChunk(Uint8List.fromList([1])));
        await Future<void>.delayed(Duration.zero);
        expect(controller.state, ConversationState.speaking);

        await controller.stopSession();

        expect(controller.state, ConversationState.disconnected);
        expect(mic.stopCalled, isTrue);
        expect(player.stopTurnCalls, 1);

        final startFuture = controller.startSession();
        await Future<void>.delayed(Duration.zero);
        live.emitEventForTest(const GeminiSetupComplete());
        await startFuture;

        expect(controller.state, ConversationState.ready);
        expect(live.connectCallCount, 2);
      },
    );

    test('stopSession is a no-op while already disconnected', () async {
      await controller.stopSession();
      expect(controller.state, ConversationState.disconnected);
      expect(live.connectCalled, isFalse);
    });

    test('suspendForOverlay/resumeFromOverlay never auto-start the microphone '
        'for push-to-talk — a press does that, as always', () async {
      final pressFuture = controller.onPressStart();
      await Future<void>.delayed(Duration.zero);
      live.emitEventForTest(const GeminiSetupComplete());
      await pressFuture;
      await controller.onPressEnd();
      expect(controller.state, ConversationState.waiting);

      await controller.suspendForOverlay();
      expect(controller.isSuspended, isTrue);
      expect(controller.state, ConversationState.ready);

      await controller.resumeFromOverlay();
      expect(controller.isSuspended, isFalse);
      expect(mic.startCallCount, 1);
      expect(controller.state, ConversationState.ready);
    });
  });

  group('free conversation', () {
    late _FakeGeminiLiveService live;
    late _FakeMicrophoneService mic;
    late _FakePcmAudioPlayer player;
    late VoiceSessionController controller;

    setUp(() {
      live = _FakeGeminiLiveService();
      mic = _FakeMicrophoneService();
      player = _FakePcmAudioPlayer();
      controller = VoiceSessionController(
        resolveAuth: () async => const GeminiApiKeyAuth('test-key'),
        systemInstruction: 'system instruction',
        interactionMode: InteractionMode.freeConversation,
        liveService: live,
        microphoneService: mic,
        audioPlayer: player,
      );
    });

    Future<void> connectAndSettle() async {
      final future = controller.startFreeConversation();
      await Future<void>.delayed(Duration.zero);
      live.emitEventForTest(const GeminiSetupComplete());
      await future;
      await Future<void>.delayed(Duration.zero);
    }

    test('connect uses automatic VAD (freeConversation)', () async {
      await connectAndSettle();

      expect(live.lastInteractionMode, InteractionMode.freeConversation);
    });

    test(
      'starts the microphone automatically once connected, with no press',
      () async {
        await connectAndSettle();

        expect(mic.startCalled, isTrue);
        expect(controller.isMicrophoneActive, isTrue);
        expect(controller.state, ConversationState.listening);
      },
    );

    test('never sends manual activityStart/activityEnd', () async {
      await connectAndSettle();

      live.emitEventForTest(GeminiAudioChunk(Uint8List.fromList([1])));
      live.emitEventForTest(const GeminiTurnComplete());
      await Future<void>.delayed(Duration.zero);

      expect(live.activityStartSent, isFalse);
      expect(live.activityEndSent, isFalse);
    });

    test('onPressStart/onPressEnd (the PTT touch handlers) assert if called on '
        'a free-conversation controller — RobotFaceScreen is responsible for '
        'never wiring them up to touch in this mode, and this is the '
        'debug-mode backstop against doing so accidentally', () async {
      await connectAndSettle();

      expect(() => controller.onPressEnd(), throwsA(isA<AssertionError>()));
      expect(live.activityEndSent, isFalse);
    });

    test('the microphone stays active across multiple turns — never '
        'stopped/restarted between them', () async {
      await connectAndSettle();
      expect(mic.startCallCount, 1);

      live.emitEventForTest(GeminiAudioChunk(Uint8List.fromList([1])));
      live.emitEventForTest(const GeminiTurnComplete());
      await Future<void>.delayed(Duration.zero);
      expect(controller.state, ConversationState.listening);
      expect(mic.stopCalled, isFalse);

      live.emitEventForTest(GeminiAudioChunk(Uint8List.fromList([2])));
      live.emitEventForTest(const GeminiTurnComplete());
      await Future<void>.delayed(Duration.zero);

      expect(controller.state, ConversationState.listening);
      expect(mic.stopCalled, isFalse);
      // Still only the one, original mic.start() call.
      expect(mic.startCallCount, 1);
    });

    test('interrupted stops playback and returns to listening (not ready) '
        'without touching the microphone', () async {
      await connectAndSettle();
      live.emitEventForTest(GeminiAudioChunk(Uint8List.fromList([1])));
      await Future<void>.delayed(Duration.zero);
      expect(controller.state, ConversationState.speaking);

      live.emitEventForTest(const GeminiInterrupted());
      await Future<void>.delayed(Duration.zero);

      expect(controller.state, ConversationState.listening);
      expect(player.stopTurnCalls, 1);
      expect(controller.lastInterruptionAt, isNotNull);
      expect(mic.stopCalled, isFalse);
    });

    test('leaving the activity (dispose) stops the microphone', () async {
      await connectAndSettle();

      controller.dispose();
      await Future<void>.delayed(Duration.zero);

      expect(mic.stopCalled, isTrue);
    });

    test('connection loss stops the microphone and does not leave a duplicate '
        'subscription on reconnect', () async {
      await connectAndSettle();
      expect(mic.startCallCount, 1);

      live.emitEventForTest(const GeminiClosed(reason: 'boom'));
      await Future<void>.delayed(Duration.zero);
      expect(controller.state, ConversationState.disconnected);
      expect(mic.stopCalled, isTrue);
      expect(controller.isMicrophoneActive, isFalse);

      // Tap-to-retry: startFreeConversation() is the same entry point
      // used for the initial connect.
      await connectAndSettle();

      expect(live.connectCallCount, 2);
      expect(mic.startCallCount, 2);
      expect(controller.state, ConversationState.listening);

      // The critical assertion: a chunk sent now must only be handled
      // once. If the old event listener wasn't cancelled on reconnect,
      // this single audio chunk would trigger `startTurn`/state changes
      // twice.
      live.emitEventForTest(GeminiAudioChunk(Uint8List.fromList([9])));
      await Future<void>.delayed(Duration.zero);
      expect(player.startTurnCalls, 1);
    });

    test('respects the InteractionMode it was constructed with', () {
      expect(controller.interactionMode, InteractionMode.freeConversation);
    });

    test('stopSession fully disconnects — mic off and no reaction to a chunk '
        'the server might still send in flight — startSession resumes '
        'continuous listening', () async {
      await connectAndSettle();
      expect(mic.startCallCount, 1);

      await controller.stopSession();

      expect(controller.state, ConversationState.disconnected);
      expect(mic.stopCalled, isTrue);
      expect(controller.isMicrophoneActive, isFalse);

      final startFuture = controller.startSession();
      await Future<void>.delayed(Duration.zero);
      live.emitEventForTest(const GeminiSetupComplete());
      await startFuture;
      await Future<void>.delayed(Duration.zero);

      expect(controller.state, ConversationState.listening);
      expect(mic.startCallCount, 2);
    });

    test('suspendForOverlay stops the microphone and drops audio from a turn '
        'already in flight instead of playing it', () async {
      await connectAndSettle();
      live.emitEventForTest(GeminiAudioChunk(Uint8List.fromList([1])));
      await Future<void>.delayed(Duration.zero);
      expect(controller.state, ConversationState.speaking);

      await controller.suspendForOverlay();

      expect(controller.isSuspended, isTrue);
      expect(mic.stopCalled, isTrue);
      expect(controller.isMicrophoneActive, isFalse);
      expect(player.stopTurnCalls, 1);
      expect(controller.state, ConversationState.ready);

      // A trailing chunk from the turn that was in flight when the
      // overlay opened must be dropped, not fed to the player.
      live.emitEventForTest(GeminiAudioChunk(Uint8List.fromList([2])));
      await Future<void>.delayed(Duration.zero);
      expect(controller.state, ConversationState.ready);
      expect(player.startTurnCalls, 1);
    });

    test('resumeFromOverlay restarts continuous listening without creating a '
        'second microphone stream', () async {
      await connectAndSettle();
      expect(mic.startCallCount, 1);

      await controller.suspendForOverlay();
      await controller.resumeFromOverlay();

      expect(controller.isSuspended, isFalse);
      expect(controller.state, ConversationState.listening);
      expect(mic.startCallCount, 2);
    });

    test(
      'resumeFromOverlay is a no-op if the connection dropped while '
      'suspended — the existing retry path handles reconnecting instead',
      () async {
        await connectAndSettle();
        await controller.suspendForOverlay();

        live.emitEventForTest(const GeminiClosed(reason: 'boom'));
        await Future<void>.delayed(Duration.zero);
        expect(controller.state, ConversationState.disconnected);

        await controller.resumeFromOverlay();

        expect(controller.isSuspended, isFalse);
        expect(controller.state, ConversationState.disconnected);
        expect(mic.startCallCount, 1);
      },
    );
  });

  test('a push-to-talk and a free-conversation controller do not interfere '
      'with each other', () async {
    final pttLive = _FakeGeminiLiveService();
    final pttController = VoiceSessionController(
      resolveAuth: () async => const GeminiApiKeyAuth('k'),
      systemInstruction: 'ptt',
      interactionMode: InteractionMode.pushToTalk,
      liveService: pttLive,
      microphoneService: _FakeMicrophoneService(),
      audioPlayer: _FakePcmAudioPlayer(),
    );

    final freeLive = _FakeGeminiLiveService();
    final freeMic = _FakeMicrophoneService();
    final freeController = VoiceSessionController(
      resolveAuth: () async => const GeminiApiKeyAuth('k'),
      systemInstruction: 'free',
      interactionMode: InteractionMode.freeConversation,
      liveService: freeLive,
      microphoneService: freeMic,
      audioPlayer: _FakePcmAudioPlayer(),
    );

    final freeFuture = freeController.startFreeConversation();
    await Future<void>.delayed(Duration.zero);
    freeLive.emitEventForTest(const GeminiSetupComplete());
    await freeFuture;
    await Future<void>.delayed(Duration.zero);

    // The PTT controller was never touched — still fully idle.
    expect(pttController.state, ConversationState.disconnected);
    expect(pttLive.connectCalled, isFalse);

    expect(freeController.state, ConversationState.listening);
    expect(freeMic.startCalled, isTrue);
  });
}
