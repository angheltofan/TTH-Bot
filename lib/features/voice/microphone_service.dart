import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:record/record.dart';

import 'android_audio_routing.dart';

/// Wraps the `record` package to capture microphone audio as raw
/// little-endian signed 16-bit PCM, mono, 16kHz — exactly what the Gemini
/// Live API expects for realtime audio input, so no extra resampling layer
/// is needed here.
///
/// Audio processing note: [AndroidAudioSource.voiceCommunication] routes
/// capture through Android's voice-call audio path, and
/// [AudioManagerMode.modeInCommunication] puts the audio manager in the
/// matching mode — together these are what let the device's native AEC/NS/AGC
/// engage for a VoIP-style call. The `echoCancel` / `noiseSuppress` /
/// `autoGain` flags below additionally request the platform
/// AcousticEchoCanceler / NoiseSuppressor / AutomaticGainControl audio
/// effects. Whether any of this actually activates depends on what the
/// specific device's audio HAL supports — the `record` plugin does not
/// expose a way to confirm at runtime that an effect actually engaged, so
/// this is a best-effort request, not a guarantee.
///
/// ROUTING NOTE (Phase 4B fix): [AndroidRecordConfig.speakerphone] is set
/// to `true` below. Without it, `modeInCommunication` alone tends to route
/// audio to the built-in earpiece rather than the loudspeaker for as long
/// as the mic is active — invisible in push-to-talk (the mic stops before
/// Gemini replies) but very audible in free conversation (the mic never
/// stops), where it made responses barely audible. See
/// AndroidAudioRouting for the accompanying diagnostics/reinforcement.
class MicrophoneService {
  MicrophoneService();

  // Created lazily (rather than in the constructor) so that a test double
  // which overrides every method below never touches the platform channel.
  AudioRecorder? _recorderInstance;
  AudioRecorder get _recorder => _recorderInstance ??= AudioRecorder();

  static const RecordConfig _config = RecordConfig(
    encoder: AudioEncoder.pcm16bits,
    sampleRate: 16000,
    numChannels: 1,
    autoGain: true,
    echoCancel: true,
    noiseSuppress: true,
    androidConfig: AndroidRecordConfig(
      audioSource: AndroidAudioSource.voiceCommunication,
      audioManagerMode: AudioManagerMode.modeInCommunication,
      speakerphone: true,
    ),
  );

  /// Checks for microphone permission, requesting it from the OS if not yet
  /// granted or denied.
  Future<bool> hasPermission() => _recorder.hasPermission();

  /// Starts streaming raw PCM16 mono 16kHz chunks from the microphone.
  Future<Stream<Uint8List>> start() async {
    final stream = await _recorder.startStream(_config);
    debugPrint('[Microphone] started (pcm16, 16kHz, mono)');
    // `_config.androidConfig.speakerphone` (above) is the actual fix,
    // applied internally by `record` as part of startStream(). This just
    // reinforces it on API 31+ and logs what Android reports, so the fix
    // is verified rather than assumed — see AndroidAudioRouting.
    await AndroidAudioRouting.logRoutingInfo(
      'mic started, before forceSpeaker',
    );
    await AndroidAudioRouting.forceSpeaker();
    await AndroidAudioRouting.logRoutingInfo('mic started, after forceSpeaker');
    return stream;
  }

  Future<void> stop() async {
    await _recorder.stop();
    debugPrint('[Microphone] stopped');
    await AndroidAudioRouting.clearForcedRouting();
  }

  Future<void> dispose() async {
    await _recorderInstance?.dispose();
  }
}
