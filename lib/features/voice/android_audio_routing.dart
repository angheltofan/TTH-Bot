import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

/// DEVELOPMENT-ONLY diagnostics (plus a small reinforcing fix) for Android
/// audio routing during a voice session.
///
/// Why: `record`'s `voiceCommunication` audio source + `modeInCommunication`
/// audio-manager mode (see MicrophoneService) — needed while the mic is
/// active — can leave playback routed to the quiet earpiece instead of the
/// loudspeaker for as long as the mode is active. In push-to-talk that's
/// invisible (the mic, and that mode, stop before Gemini responds); in
/// free conversation the mic never stops, so it isn't. The primary fix is
/// MicrophoneService's `speakerphone: true`, handled entirely by the
/// `record` plugin. This class exists to (a) log what Android's
/// AudioManager actually reports, so that fix is verified rather than
/// assumed, and (b) on API 31+, reinforce it with the modern
/// `setCommunicationDevice` API. See AudioRoutingChannel.kt for the native
/// side — neither `record` nor `flutter_soloud` exposes this state, so a
/// tiny platform channel is the only way to see it from Dart.
class AndroidAudioRouting {
  const AndroidAudioRouting._();

  static const MethodChannel _channel = MethodChannel('edubot/audio_routing');

  /// Logs the current AudioManager mode, speakerphone flag, and (API 31+)
  /// communication device. Call before and after [forceSpeaker] to verify
  /// a routing change actually took effect rather than assuming it did
  /// because the call didn't throw.
  static Future<void> logRoutingInfo(String context) async {
    if (!Platform.isAndroid) return;
    try {
      final info = await _channel.invokeMapMethod<String, Object?>(
        'getRoutingInfo',
      );
      debugPrint('[AudioRouting] $context: $info');
    } catch (e) {
      debugPrint('[AudioRouting] $context: query failed ($e)');
    }
  }

  /// Best-effort request to route communication audio to the built-in
  /// loudspeaker. On API 31+ this uses the modern
  /// `AudioManager.setCommunicationDevice`; below that, this is a no-op —
  /// `MicrophoneService`'s `speakerphone: true` (via `record`) is the only
  /// mechanism available there. Never throws.
  static Future<void> forceSpeaker() async {
    if (!Platform.isAndroid) return;
    try {
      final applied = await _channel.invokeMethod<bool>('forceSpeakerRouting');
      debugPrint('[AudioRouting] forceSpeaker requested, applied=$applied');
    } catch (e) {
      debugPrint('[AudioRouting] forceSpeaker failed: $e');
    }
  }

  /// Clears a routing override requested by [forceSpeaker]. A no-op if
  /// none was applied (e.g. below API 31).
  static Future<void> clearForcedRouting() async {
    if (!Platform.isAndroid) return;
    try {
      await _channel.invokeMethod<void>('clearForcedRouting');
    } catch (e) {
      debugPrint('[AudioRouting] clearForcedRouting failed: $e');
    }
  }
}
