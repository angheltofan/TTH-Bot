import 'dart:math';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:tth_bot/features/voice/pcm_levels.dart';

Uint8List _pcm16(List<int> samples) {
  final bytes = ByteData(samples.length * 2);
  for (var i = 0; i < samples.length; i++) {
    bytes.setInt16(i * 2, samples[i], Endian.little);
  }
  return bytes.buffer.asUint8List();
}

void main() {
  group('computePcmLevels', () {
    test('returns zero for an empty chunk', () {
      final levels = computePcmLevels(Uint8List(0));

      expect(levels.rms, 0);
      expect(levels.peak, 0);
    });

    test('returns zero for pure silence', () {
      final levels = computePcmLevels(_pcm16([0, 0, 0, 0]));

      expect(levels.rms, 0);
      expect(levels.peak, 0);
    });

    test('a full-scale constant chunk reads rms and peak near 1.0', () {
      final levels = computePcmLevels(_pcm16([32767, 32767, 32767]));

      expect(levels.rms, closeTo(1.0, 0.001));
      expect(levels.peak, closeTo(1.0, 0.001));
    });

    test('peak reflects the single loudest sample, not the average', () {
      final levels = computePcmLevels(_pcm16([100, 100, 32767, 100]));

      expect(levels.peak, closeTo(1.0, 0.001));
      expect(levels.rms, lessThan(levels.peak));
    });

    test('rms of a known signal matches the textbook formula', () {
      // Normalized samples are 0 and ~1.0; rms = sqrt((0^2 + 1^2) / 2).
      final levels = computePcmLevels(_pcm16([0, 32767]));

      expect(levels.rms, closeTo(sqrt(0.5), 0.001));
    });

    test('negative samples contribute the same magnitude as positive ones', () {
      final positive = computePcmLevels(_pcm16([16000]));
      final negative = computePcmLevels(_pcm16([-16000]));

      expect(negative.rms, closeTo(positive.rms, 0.001));
      expect(negative.peak, closeTo(positive.peak, 0.001));
    });

    test('an odd trailing byte is ignored rather than crashing', () {
      final evenChunk = _pcm16([1000, 2000]);
      final oddChunk = Uint8List.fromList([...evenChunk, 0xFF]);

      expect(() => computePcmLevels(oddChunk), returnsNormally);
      final fromOdd = computePcmLevels(oddChunk);
      final fromEven = computePcmLevels(evenChunk);
      expect(fromOdd.rms, fromEven.rms);
      expect(fromOdd.peak, fromEven.peak);
    });

    test('toString includes both rms and peak, 3 decimal places', () {
      const levels = PcmLevels(rms: 0.5, peak: 0.75);

      expect(levels.toString(), 'rms=0.500 peak=0.750');
    });
  });
}
