import 'package:flutter_test/flutter_test.dart';
import 'package:tth_bot/features/voice/pcm_audio_player.dart';

void main() {
  // These exercise PcmAudioPlayer without ever calling startTurn()/feed(),
  // which would reach the real flutter_soloud engine (no platform bindings
  // in a plain `flutter test` run). stopTurn()'s early-exit path — used
  // for barge-in interruption — only touches SoLoud when a stream is
  // actually active, so it's safe and meaningful to test on a fresh
  // instance.
  group('PcmAudioPlayer.stopTurn (interruption)', () {
    test('resets speaking amplitude to zero immediately', () {
      final player = PcmAudioPlayer();
      player.amplitude.value = 0.75; // simulate mid-speech loudness

      player.stopTurn();

      expect(player.amplitude.value, 0.0);
    });

    test('is safe to call with no active turn', () {
      final player = PcmAudioPlayer();

      expect(player.stopTurn, returnsNormally);
    });
  });
}
