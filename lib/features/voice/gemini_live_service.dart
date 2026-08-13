import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';

import '../activities/activity.dart';
import 'gemini_auth.dart';

/// Model id per current Gemini Live API docs (ai.google.dev/api/live).
/// Do not swap this for an older `gemini-2.x-*-live` name without checking
/// the docs again — model ids on the Live API change frequently.
const String kGeminiLiveModel = 'models/gemini-3.1-flash-live-preview';

/// The one fixed prebuilt voice TTH Bot always speaks with — without this,
/// Gemini picks an unspecified voice per session, which audibly changed
/// between male- and female-sounding voices across restarts.
///
/// "Puck" is documented (ai.google.dev/gemini-api/docs/speech-generation,
/// prebuilt voices table) as the "Upbeat" style, matching the energetic
/// educational personality TTH Bot should have. Centralized here rather
/// than repeated as a string literal — see [realtimeInputConfigFor] for
/// the same pattern applied to VAD config.
const String kTthBotVoiceName = 'Puck';

/// Builds the `generationConfig` block of the setup message: audio-only
/// response modality plus the fixed prebuilt voice — same value every
/// call, for every [InteractionMode], so the voice can never drift between
/// sessions/restarts/activity switches. Pure function, so the exact JSON
/// is unit testable directly, same pattern as [realtimeInputConfigFor].
///
/// Schema confirmed against the current official Live API voice-config
/// example (ai.google.dev/gemini-api/docs/live-guide) and the raw
/// `BidiGenerateContentSetup`/`GenerationConfig` reference
/// (ai.google.dev/api/live) — `speechConfig` is a field of
/// `generationConfig`, holding `voiceConfig.prebuiltVoiceConfig.voiceName`.
Map<String, dynamic> geminiGenerationConfig() {
  return {
    'responseModalities': ['AUDIO'],
    'speechConfig': {
      'voiceConfig': {
        'prebuiltVoiceConfig': {'voiceName': kTthBotVoiceName},
      },
    },
  };
}

const String _kWebSocketHost =
    'wss://generativelanguage.googleapis.com/ws/'
    'google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent';

/// Centralized automatic-VAD tuning for [InteractionMode.freeConversation].
/// Kept in one place (rather than scattered through setup-message code) so
/// it can be re-tuned after physical testing without hunting for it.
///
/// Every field/enum value here is verified against the current official
/// reference (ai.google.dev/api/live) and guide
/// (ai.google.dev/gemini-api/docs/live-guide) — not an older/cached
/// example — per this task's explicit requirement, after the earlier
/// `thinkingConfig` incident where an outdated example caused a real
/// server-side schema rejection.
class GeminiVadConfig {
  const GeminiVadConfig._();

  /// "Automatic detection will detect the start of speech less often"
  /// (official wording) — the conservative choice: favors not
  /// false-triggering on room noise over shaving a few ms off reaction
  /// time. See the Test C requirement (observe false activations from
  /// room noise) this is meant to reduce.
  static const String startOfSpeechSensitivity = 'START_SENSITIVITY_LOW';

  /// "Automatic detection ends speech less often" — waits for clearer
  /// silence before deciding the child has finished talking, so a
  /// mid-sentence thinking pause doesn't cut them off. Matches this task's
  /// explicit priority: favor complete utterances over ultra-aggressive
  /// latency.
  static const String endOfSpeechSensitivity = 'END_SENSITIVITY_LOW';

  /// Pre-roll audio included *before* the detected speech onset. Per the
  /// official guide: this "look-back ensures the model captures the full
  /// onset of speech, including the first syllable which may start before
  /// the VAD triggers. A value of 0 may cause the beginning of words to be
  /// clipped." 200ms is a generous safety margin — it costs nothing
  /// perceptible (it's already-captured local audio being replayed, not
  /// added wall-clock delay).
  static const int prefixPaddingMs = 200;

  /// Required silence before end-of-speech is committed. This task's
  /// explicit initial value — also within the official guide's own
  /// "500ms–800ms: complete, contextually rich audio chunks" recommended
  /// range, well above the 100-200ms range the guide warns fragments
  /// natural pauses.
  static const int silenceDurationMs = 700;
}

/// Server -> client events, decoded from the raw WebSocket JSON frames.
@immutable
sealed class GeminiLiveEvent {
  const GeminiLiveEvent();
}

class GeminiSetupComplete extends GeminiLiveEvent {
  const GeminiSetupComplete();
}

class GeminiAudioChunk extends GeminiLiveEvent {
  const GeminiAudioChunk(this.pcm16);
  final Uint8List pcm16;
}

class GeminiTurnComplete extends GeminiLiveEvent {
  const GeminiTurnComplete();
}

class GeminiInterrupted extends GeminiLiveEvent {
  const GeminiInterrupted();
}

class GeminiServerError extends GeminiLiveEvent {
  const GeminiServerError(this.message);
  final String message;
}

class GeminiClosed extends GeminiLiveEvent {
  const GeminiClosed({this.code, this.reason});
  final int? code;
  final String? reason;
}

/// Builds the `realtimeInputConfig` block of the setup message for [mode].
///
/// Push-to-talk: manual activity control — we drive activityStart/
/// activityEnd from the button instead of relying on server-side VAD.
///
/// Free conversation: automatic server-side VAD (see [GeminiVadConfig])
/// plus `activityHandling: START_OF_ACTIVITY_INTERRUPTS` — confirmed
/// current field/value in the official reference — so the child starting
/// to talk interrupts TTH Bot's response for natural barge-in.
///
/// Pure function (like [parseGeminiServerMessage]) so the exact JSON sent
/// for each mode can be unit tested directly.
Map<String, dynamic> realtimeInputConfigFor(InteractionMode mode) {
  switch (mode) {
    case InteractionMode.pushToTalk:
      return {
        'automaticActivityDetection': {'disabled': true},
      };
    case InteractionMode.freeConversation:
      return {
        'automaticActivityDetection': {
          'disabled': false,
          'startOfSpeechSensitivity': GeminiVadConfig.startOfSpeechSensitivity,
          'endOfSpeechSensitivity': GeminiVadConfig.endOfSpeechSensitivity,
          'prefixPaddingMs': GeminiVadConfig.prefixPaddingMs,
          'silenceDurationMs': GeminiVadConfig.silenceDurationMs,
        },
        'activityHandling': 'START_OF_ACTIVITY_INTERRUPTS',
      };
  }
}

/// Parses one decoded server JSON message into zero or more events.
///
/// Pure function (no I/O) so it can be unit tested directly against sample
/// payloads shaped like the ones documented at ai.google.dev/api/live.
List<GeminiLiveEvent> parseGeminiServerMessage(Map<String, dynamic> json) {
  if (json.containsKey('setupComplete')) {
    return const [GeminiSetupComplete()];
  }

  final events = <GeminiLiveEvent>[];

  final serverContent = json['serverContent'];
  if (serverContent is Map<String, dynamic>) {
    final modelTurn = serverContent['modelTurn'];
    if (modelTurn is Map<String, dynamic>) {
      final parts = modelTurn['parts'];
      if (parts is List) {
        for (final part in parts) {
          if (part is! Map<String, dynamic>) continue;
          final inlineData = part['inlineData'];
          if (inlineData is! Map<String, dynamic>) continue;
          final mimeType = inlineData['mimeType'];
          final data = inlineData['data'];
          if (mimeType is String &&
              mimeType.startsWith('audio/') &&
              data is String) {
            events.add(GeminiAudioChunk(base64Decode(data)));
          }
        }
      }
    }
    if (serverContent['interrupted'] == true) {
      events.add(const GeminiInterrupted());
    }
    if (serverContent['turnComplete'] == true) {
      events.add(const GeminiTurnComplete());
    }
    return events;
  }

  final error = json['error'];
  if (error != null) {
    final message = error is Map
        ? (error['message']?.toString() ?? error.toString())
        : error.toString();
    events.add(GeminiServerError(message));
  }

  return events;
}

/// Dedicated WebSocket client for the Gemini Live API. Holds no UI state —
/// [VoiceSessionController] owns the conversation state machine and reacts
/// to the [events] stream this class exposes.
class GeminiLiveService {
  GeminiLiveService();

  WebSocket? _socket;
  final StreamController<GeminiLiveEvent> _eventsController =
      StreamController<GeminiLiveEvent>.broadcast();

  /// Server events, including connection lifecycle ([GeminiClosed],
  /// [GeminiServerError]) and content events.
  Stream<GeminiLiveEvent> get events => _eventsController.stream;

  bool get isConnected => _socket != null;

  /// Injects an event as if it came from the server. Lets tests exercise
  /// [VoiceSessionController]'s reaction to server events without a real
  /// WebSocket connection.
  @visibleForTesting
  void emitEventForTest(GeminiLiveEvent event) => _eventsController.add(event);

  /// Opens the WebSocket and sends the BidiGenerateContentSetup message.
  ///
  /// This does NOT wait for `setupComplete` — listen on [events] for a
  /// [GeminiSetupComplete] event before sending any audio.
  ///
  /// [auth] is resolved by the caller (see `resolveGeminiAuth`), which
  /// prefers a [GeminiEphemeralTokenAuth] and only falls back to a raw
  /// [GeminiApiKeyAuth] for local development — this method itself doesn't
  /// care which mode it got, it just appends [GeminiAuth.queryParameter]
  /// to the connection URL.
  ///
  /// [interactionMode] selects manual vs. automatic server-side activity
  /// detection for the whole session (see [_realtimeInputConfig]) — it
  /// cannot be changed after setup, per the API, so a mode switch means a
  /// fresh [VoiceSessionController]/connection, not a reconfigured one.
  Future<void> connect({
    required GeminiAuth auth,
    required String systemInstruction,
    required InteractionMode interactionMode,
  }) async {
    if (_socket != null) {
      throw StateError('GeminiLiveService is already connected');
    }

    final uri = Uri.parse('$_kWebSocketHost?${auth.queryParameter}');
    final socket = await WebSocket.connect(uri.toString());
    _socket = socket;
    debugPrint('[GeminiLive] WebSocket connected');

    socket.listen(_onData, onError: _onError, onDone: _onDone);

    final setupMessage = <String, dynamic>{
      'setup': {
        'model': kGeminiLiveModel,
        'generationConfig': geminiGenerationConfig(),
        // NOTE: a client-configurable thinking depth (thinkingConfig /
        // thinkingLevel) was attempted here per the task's "use
        // lowest-latency thinking where supported" instruction, but the
        // live server rejects it: "Invalid JSON payload received. Unknown
        // name \"thinkingConfig\" at 'setup': Cannot find field." The
        // current v1beta BidiGenerateContentSetup message (per
        // ai.google.dev/api/live) has no thinking-related field at all —
        // confirmed against both the raw reference doc and the live
        // WebSocket server, not just AI-summarized docs. So there is
        // nothing to configure here; this is not an oversight.
        'systemInstruction': {
          'parts': [
            {'text': systemInstruction},
          ],
        },
        'realtimeInputConfig': realtimeInputConfigFor(interactionMode),
      },
    };
    socket.add(jsonEncode(setupMessage));
    debugPrint('[GeminiLive] setup sent (mode: ${interactionMode.name})');
  }

  void sendActivityStart() {
    _send({
      'realtimeInput': {'activityStart': {}},
    });
    debugPrint('[GeminiLive] activityStart sent');
  }

  void sendActivityEnd() {
    _send({
      'realtimeInput': {'activityEnd': {}},
    });
    debugPrint('[GeminiLive] activityEnd sent');
  }

  /// Sends one chunk of little-endian signed 16-bit PCM mono audio,
  /// sampled at 16kHz, as required by the Live API's realtime input.
  /// Silently ignores empty chunks — an empty payload is never valid audio
  /// and isn't worth a round trip.
  void sendAudioChunk(Uint8List pcm16) {
    if (pcm16.isEmpty) return;
    _send({
      'realtimeInput': {
        'audio': {
          'mimeType': 'audio/pcm;rate=16000',
          'data': base64Encode(pcm16),
        },
      },
    });
  }

  void _send(Map<String, dynamic> message) {
    final socket = _socket;
    if (socket == null) {
      debugPrint('[GeminiLive] dropped message: not connected');
      return;
    }
    socket.add(jsonEncode(message));
  }

  void _onData(dynamic message) {
    // The socket can deliver a queued frame after dispose() has already
    // closed _eventsController (e.g. a stop/switch-activity racing with an
    // in-flight WebSocket callback) — a stale event at that point, not a
    // bug to propagate, so it's dropped rather than crashing with
    // "Cannot add new events after calling close".
    if (_eventsController.isClosed) return;
    final String text;
    if (message is String) {
      text = message;
    } else if (message is List<int>) {
      text = utf8.decode(message);
    } else {
      debugPrint(
        '[GeminiLive] ignored unsupported frame type: '
        '${message.runtimeType}',
      );
      return;
    }

    final Map<String, dynamic> json;
    try {
      json = jsonDecode(text) as Map<String, dynamic>;
    } catch (e) {
      debugPrint('[GeminiLive] failed to decode server message: $e');
      return;
    }

    for (final event in parseGeminiServerMessage(json)) {
      switch (event) {
        case GeminiSetupComplete():
          debugPrint('[GeminiLive] setupComplete');
        case GeminiAudioChunk(:final pcm16):
          debugPrint(
            '[GeminiLive] audio chunk received (${pcm16.length} '
            'bytes)',
          );
        case GeminiTurnComplete():
          debugPrint('[GeminiLive] turnComplete');
        case GeminiInterrupted():
          debugPrint('[GeminiLive] interrupted');
        case GeminiServerError():
          debugPrint('[GeminiLive] server error event');
        case GeminiClosed():
          break;
      }
      _eventsController.add(event);
    }
  }

  void _onError(Object error) {
    debugPrint('[GeminiLive] WebSocket error: $error');
    if (_eventsController.isClosed) return;
    _eventsController.add(GeminiServerError(error.toString()));
  }

  void _onDone() {
    final code = _socket?.closeCode;
    final reason = _socket?.closeReason;
    debugPrint(
      '[GeminiLive] WebSocket closed (code: $code, reason: '
      '$reason)',
    );
    _socket = null;
    if (_eventsController.isClosed) return;
    _eventsController.add(GeminiClosed(code: code, reason: reason));
  }

  /// Clean disconnect. Safe to call even if never connected.
  Future<void> disconnect() async {
    await _socket?.close();
    _socket = null;
  }

  Future<void> dispose() async {
    await disconnect();
    await _eventsController.close();
  }
}
