// System-instruction composition — a port of the Flutter app's
// composeSystemInstruction (lib/features/activities/prompt_composer.dart):
//
//   base prompt (trimmed) + activity prompt (trimmed)
//   + "participants in exact order: A, B, C." when there are participants,
//   joined by blank lines.
//
// The base prompt text comes from a file generated at build time from the
// Dart source; tests/prompt_parity_test.ts pins both the text and this
// composition's pieces to the Dart sources.

import { EDUBOT_BASE_PROMPT } from "./generated/edubot_base_prompt.ts";

export const PARTICIPANTS_PREFIX =
  "Copiii participanți la această activitate, în ordinea exactă în care trebuie să te adresezi lor: ";
export const PARTICIPANTS_SEPARATOR = ", ";
export const SECTION_SEPARATOR = "\n\n";

export interface PromptActivity {
  prompt: string;
  participants: readonly string[];
}

export function composeSystemInstruction(activity: PromptActivity): string {
  const sections = [EDUBOT_BASE_PROMPT.trim(), activity.prompt.trim()];
  if (activity.participants.length > 0) {
    sections.push(
      `${PARTICIPANTS_PREFIX}${activity.participants.join(PARTICIPANTS_SEPARATOR)}.`,
    );
  }
  return sections.join(SECTION_SEPARATOR);
}
