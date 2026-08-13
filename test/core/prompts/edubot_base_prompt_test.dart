import 'package:flutter_test/flutter_test.dart';
import 'package:tth_bot/core/prompts/edubot_base_prompt.dart';

void main() {
  group('eduBotBasePrompt personality', () {
    test('names TTH Bot, not the old EduBot branding', () {
      expect(eduBotBasePrompt, contains('TTH Bot'));
      expect(eduBotBasePrompt, isNot(contains('EduBot')));
    });

    test('instructs a warm, cheerful, energetic, encouraging personality', () {
      expect(eduBotBasePrompt, contains('calde'));
      expect(eduBotBasePrompt, contains('vesele'));
      expect(eduBotBasePrompt, contains('energie'));
      expect(eduBotBasePrompt, contains('încurajatoare'));
    });

    test('gives varied example phrases for a correct answer', () {
      expect(eduBotBasePrompt, contains('Bravo!'));
      expect(eduBotBasePrompt, contains('Exact!'));
      expect(eduBotBasePrompt, contains('Foarte bine!'));
    });

    test('explicitly forbids sounding disappointed for a wrong answer, and '
        'gives supportive example phrases instead', () {
      expect(eduBotBasePrompt, contains('niciodată dezamăgit'));
      expect(eduBotBasePrompt, contains('Hai să mai încercăm!'));
    });

    test('asks for concise responses, not long lectures by default', () {
      expect(eduBotBasePrompt, contains('concise'));
    });

    test('keeps the existing safety rules intact', () {
      expect(
        eduBotBasePrompt,
        contains('Nu cere niciodată informații personale'),
      );
      expect(eduBotBasePrompt, contains('ești un robot'));
      expect(
        eduBotBasePrompt,
        contains('Nu genera niciodată conținut nepotrivit'),
      );
    });

    test('keeps the Romanian-by-default language rule', () {
      expect(eduBotBasePrompt, contains('limba română'));
    });
  });
}
