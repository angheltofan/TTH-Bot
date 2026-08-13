import 'dart:math';
import 'dart:typed_data';

/// Normalized (0..1) loudness of one PCM16 audio chunk.
///
/// Shared by [PcmAudioPlayer]'s mouth-amplitude computation and the
/// `[AudioDebug]` diagnostic logging in `VoiceSessionController` — one
/// implementation of "how loud is this chunk", so the two can't disagree.
class PcmLevels {
  const PcmLevels({required this.rms, required this.peak});

  static const PcmLevels zero = PcmLevels(rms: 0, peak: 0);

  /// Root-mean-square level, 0 (silence) to 1 (full-scale).
  final double rms;

  /// Highest single-sample magnitude, 0 to 1.
  final double peak;

  @override
  String toString() =>
      'rms=${rms.toStringAsFixed(3)} peak=${peak.toStringAsFixed(3)}';
}

/// Computes [PcmLevels] for one chunk of little-endian signed 16-bit PCM
/// audio. Pure/no I/O, so it's directly unit testable.
PcmLevels computePcmLevels(Uint8List pcm16) {
  final sampleCount = pcm16.length ~/ 2;
  if (sampleCount == 0) return PcmLevels.zero;

  final samples = pcm16.buffer.asByteData(pcm16.offsetInBytes, sampleCount * 2);

  double sumSquares = 0;
  int peakAbs = 0;
  for (var i = 0; i < sampleCount; i++) {
    final sample = samples.getInt16(i * 2, Endian.little);
    sumSquares += sample * sample;
    final abs = sample.abs();
    if (abs > peakAbs) peakAbs = abs;
  }

  final rms = (sqrt(sumSquares / sampleCount) / 32768.0).clamp(0.0, 1.0);
  final peak = (peakAbs / 32768.0).clamp(0.0, 1.0);
  return PcmLevels(rms: rms, peak: peak);
}
