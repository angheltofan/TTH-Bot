import 'package:flutter/foundation.dart';
import 'package:flutter_soloud/flutter_soloud.dart';

import 'pcm_levels.dart';

/// Low-latency streaming playback for Gemini's audio responses.
///
/// The Live API returns raw little-endian signed 16-bit PCM mono audio at
/// 24kHz. This wraps `flutter_soloud`'s buffer-stream API, which is built
/// for exactly this "push chunks as they arrive, play immediately, don't
/// wait for the full response" scenario, so no temporary WAV files are
/// written.
///
/// One [AudioSource] buffer stream is created per Gemini turn (see
/// [startTurn]) and disposed at the start of the next one, so a single
/// [PcmAudioPlayer] instance can be reused across an entire multi-turn
/// conversation without recreating the underlying audio engine.
class PcmAudioPlayer {
  static const int _sampleRate = 24000;
  static const double _amplitudeSmoothing = 0.35;

  bool _initialized = false;
  AudioSource? _currentStream;
  bool _awaitingFirstChunk = false;

  /// Normalized (0..1) speaking loudness, for driving a mouth-open
  /// animation. Computed as the smoothed RMS of each PCM16 chunk as it's
  /// [feed]ed in — i.e. the actual audio content, not a fake/synthetic
  /// wave — so it's a real (if approximate) amplitude signal.
  ///
  /// KNOWN LIMITATION: this reflects chunks as they *arrive* over the
  /// WebSocket, not the exact sample currently coming out of the speaker.
  /// Because `startTurn` buffers ~0.2s before playback begins (see
  /// `bufferingTimeNeeds` below) and the OS audio pipeline adds its own
  /// small delay, the mouth trails true speaker output by roughly that
  /// much. Good enough for an approximate mouth-open animation; not
  /// phoneme/viseme-accurate lip sync.
  final ValueNotifier<double> amplitude = ValueNotifier<double>(0.0);

  Future<void> init() async {
    if (_initialized) return;
    await SoLoud.instance.init(
      sampleRate: _sampleRate,
      channels: Channels.mono,
    );
    _initialized = true;
    debugPrint('[PcmAudioPlayer] SoLoud engine initialized (24kHz mono)');
  }

  /// Prepares a fresh playback buffer for a new model turn. Call this once,
  /// then feed chunks to it via [feed] as they arrive from the WebSocket.
  void startTurn() {
    _disposeCurrentStream();
    _currentStream = SoLoud.instance.setBufferStream(
      sampleRate: _sampleRate,
      channels: Channels.mono,
      format: BufferType.s16le,
      bufferingType: BufferingType.released,
      // Small buffering window: low latency over perfect underrun safety,
      // acceptable tradeoff for this push-to-talk spike.
      bufferingTimeNeeds: 0.2,
    );
    _awaitingFirstChunk = true;
  }

  /// Feeds one chunk of raw PCM16 audio into the current turn's stream.
  /// Playback starts automatically on the first chunk of a turn so the user
  /// hears audio as soon as it arrives, not after the full response.
  void feed(Uint8List pcm16) {
    final stream = _currentStream;
    if (stream == null) {
      debugPrint(
        '[PcmAudioPlayer] feed() called with no active turn, '
        'ignored',
      );
      return;
    }
    SoLoud.instance.addAudioDataStream(stream, pcm16);
    if (_awaitingFirstChunk) {
      SoLoud.instance.play(stream);
      _awaitingFirstChunk = false;
      debugPrint('[PcmAudioPlayer] playback started');
    }
    _updateAmplitude(pcm16);
  }

  /// Exponentially smooths [pcm16]'s RMS level into [amplitude] so the
  /// mouth doesn't jitter chunk-to-chunk.
  void _updateAmplitude(Uint8List pcm16) {
    final rms = computePcmLevels(pcm16).rms;
    amplitude.value += _amplitudeSmoothing * (rms - amplitude.value);
  }

  /// Marks the current turn's stream as fully received. Must be called once
  /// per turn (on `turnComplete`) so the engine knows to stop after playing
  /// the buffered audio instead of waiting for more.
  void endTurn() {
    final stream = _currentStream;
    if (stream == null) return;
    SoLoud.instance.setDataIsEnded(stream);
  }

  /// Stops playback immediately, e.g. on a server `interrupted` event.
  void stopTurn() {
    _disposeCurrentStream();
  }

  void _disposeCurrentStream() {
    final stream = _currentStream;
    _currentStream = null;
    _awaitingFirstChunk = false;
    amplitude.value = 0.0;
    if (stream != null && _initialized) {
      SoLoud.instance.disposeSource(stream);
    }
  }

  Future<void> dispose() async {
    _disposeCurrentStream();
    amplitude.dispose();
    if (_initialized) {
      SoLoud.instance.deinit();
      _initialized = false;
    }
  }
}
