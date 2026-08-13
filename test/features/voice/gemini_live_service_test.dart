import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:tth_bot/features/activities/activity.dart';
import 'package:tth_bot/features/voice/gemini_live_service.dart';

void main() {
  group('geminiGenerationConfig', () {
    test('fixes the voice via speechConfig.voiceConfig.prebuiltVoiceConfig'
        '.voiceName', () {
      final config = geminiGenerationConfig();

      final speechConfig = config['speechConfig'] as Map<String, dynamic>;
      final voiceConfig = speechConfig['voiceConfig'] as Map<String, dynamic>;
      final prebuiltVoiceConfig =
          voiceConfig['prebuiltVoiceConfig'] as Map<String, dynamic>;
      expect(prebuiltVoiceConfig['voiceName'], kTthBotVoiceName);
    });

    test('uses "Puck" — documented as the "Upbeat" prebuilt voice', () {
      expect(kTthBotVoiceName, 'Puck');
    });

    test('keeps audio-only response modality', () {
      expect(geminiGenerationConfig()['responseModalities'], ['AUDIO']);
    });

    test('is identical every call — the same fixed config for every session, '
        'every InteractionMode, every reconnect; nothing makes the voice '
        'vary between push-to-talk and free conversation, since connect() '
        'calls this unconditionally regardless of mode', () {
      expect(geminiGenerationConfig(), geminiGenerationConfig());
    });
  });

  group('realtimeInputConfigFor', () {
    test('pushToTalk disables automatic activity detection (manual VAD)', () {
      final config = realtimeInputConfigFor(InteractionMode.pushToTalk);

      expect(config, {
        'automaticActivityDetection': {'disabled': true},
      });
    });

    test('freeConversation enables automatic VAD with the centralized tuning '
        'and start-of-activity interruption', () {
      final config = realtimeInputConfigFor(InteractionMode.freeConversation);

      final vad = config['automaticActivityDetection'] as Map<String, dynamic>;
      expect(vad['disabled'], isFalse);
      expect(vad['startOfSpeechSensitivity'], 'START_SENSITIVITY_LOW');
      expect(vad['endOfSpeechSensitivity'], 'END_SENSITIVITY_LOW');
      expect(vad['prefixPaddingMs'], 200);
      expect(vad['silenceDurationMs'], 700);
      expect(config['activityHandling'], 'START_OF_ACTIVITY_INTERRUPTS');
    });

    test('freeConversation never sends the manual "disabled: true" shape', () {
      final config = realtimeInputConfigFor(InteractionMode.freeConversation);
      final vad = config['automaticActivityDetection'] as Map<String, dynamic>;

      expect(vad['disabled'], isNot(true));
    });
  });

  group('parseGeminiServerMessage', () {
    test('parses setupComplete', () {
      final events = parseGeminiServerMessage({'setupComplete': {}});

      expect(events, [isA<GeminiSetupComplete>()]);
    });

    test('parses one audio chunk out of modelTurn parts', () {
      final pcm = base64Encode([1, 2, 3, 4]);
      final events = parseGeminiServerMessage({
        'serverContent': {
          'modelTurn': {
            'parts': [
              {
                'inlineData': {'mimeType': 'audio/pcm;rate=24000', 'data': pcm},
              },
            ],
          },
        },
      });

      expect(events, hasLength(1));
      final chunk = events.single as GeminiAudioChunk;
      expect(chunk.pcm16, [1, 2, 3, 4]);
    });

    test('ignores non-audio inline parts', () {
      final events = parseGeminiServerMessage({
        'serverContent': {
          'modelTurn': {
            'parts': [
              {
                'inlineData': {
                  'mimeType': 'text/plain',
                  'data': base64Encode([9]),
                },
              },
            ],
          },
        },
      });

      expect(events, isEmpty);
    });

    test('parses turnComplete', () {
      final events = parseGeminiServerMessage({
        'serverContent': {'turnComplete': true},
      });

      expect(events, [isA<GeminiTurnComplete>()]);
    });

    test('parses interrupted', () {
      final events = parseGeminiServerMessage({
        'serverContent': {'interrupted': true},
      });

      expect(events, [isA<GeminiInterrupted>()]);
    });

    test('parses an audio chunk together with turnComplete in one message', () {
      final pcm = base64Encode([5, 6]);
      final events = parseGeminiServerMessage({
        'serverContent': {
          'modelTurn': {
            'parts': [
              {
                'inlineData': {'mimeType': 'audio/pcm;rate=24000', 'data': pcm},
              },
            ],
          },
          'turnComplete': true,
        },
      });

      expect(events, hasLength(2));
      expect(events[0], isA<GeminiAudioChunk>());
      expect(events[1], isA<GeminiTurnComplete>());
    });

    test('parses a top-level error object', () {
      final events = parseGeminiServerMessage({
        'error': {'message': 'quota exceeded'},
      });

      expect(events, hasLength(1));
      final error = events.single as GeminiServerError;
      expect(error.message, 'quota exceeded');
    });

    test('returns no events for an unrecognized message shape', () {
      final events = parseGeminiServerMessage({'somethingElse': 1});

      expect(events, isEmpty);
    });
  });
}
