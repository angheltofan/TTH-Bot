import 'package:flutter_test/flutter_test.dart';
import 'package:tth_bot/features/activities/participants.dart';

void main() {
  group('parseParticipants', () {
    test('splits comma-separated names and trims them', () {
      expect(parseParticipants('Maria, Sofia,  Alex ,Ștefan'), [
        'Maria',
        'Sofia',
        'Alex',
        'Ștefan',
      ]);
    });

    test('splits newline-separated names', () {
      expect(parseParticipants('Ana\nMatei\n'), ['Ana', 'Matei']);
    });

    test('handles a mix of commas and newlines', () {
      expect(parseParticipants('Ana, Matei\nBogdan'), [
        'Ana',
        'Matei',
        'Bogdan',
      ]);
    });

    test('drops empty entries from stray commas/blank lines', () {
      expect(parseParticipants('Ana,,Matei\n\n'), ['Ana', 'Matei']);
    });

    test('returns an empty list for blank input', () {
      expect(parseParticipants(''), isEmpty);
      expect(parseParticipants('   \n  '), isEmpty);
    });

    test('preserves order and does not deduplicate or alphabetize', () {
      expect(parseParticipants('Ștefan, Alex, Ștefan'), [
        'Ștefan',
        'Alex',
        'Ștefan',
      ]);
    });
  });

  group('formatParticipants', () {
    test('joins with comma-space', () {
      expect(
        formatParticipants(['Maria', 'Sofia', 'Alex', 'Ștefan']),
        'Maria, Sofia, Alex, Ștefan',
      );
    });

    test('is the inverse of parseParticipants for already-clean input', () {
      const names = ['Ana', 'Matei'];
      expect(parseParticipants(formatParticipants(names)), names);
    });

    test('returns an empty string for an empty list', () {
      expect(formatParticipants(const []), '');
    });
  });
}
