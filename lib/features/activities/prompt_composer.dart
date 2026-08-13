import '../../core/prompts/edubot_base_prompt.dart';
import 'activity.dart';

/// Combines the base TTH Bot behavior, the activity-specific prompt, and
/// (when present) participant context into the single system instruction
/// sent to Gemini. Keeps prompt assembly out of the UI layer — screens pass
/// an [Activity] here rather than building prompt strings themselves.
String composeSystemInstruction(Activity activity) {
  final sections = <String>[
    eduBotBasePrompt.trim(),
    activity.systemPrompt.trim(),
  ];

  if (activity.participants.isNotEmpty) {
    sections.add(
      'Copiii participanți la această activitate, în ordinea exactă în '
      'care trebuie să te adresezi lor: '
      '${activity.participants.join(', ')}.',
    );
  }

  return sections.join('\n\n');
}
