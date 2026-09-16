// Explicit gateway limits (PHASE6_PLAN §4.6). Violations are refused, never
// silently truncated (except model audio beyond its cap, which ends the
// response with an explicit error).

export const LIMITS = {
  maxActivityPromptChars: 8_000,
  maxSystemInstructionChars: 12_000,
  maxParticipants: 12,
  maxParticipantNameChars: 40,
  // 46 s at 16 kHz s16le — the device's own turn buffer capacity.
  maxUserAudioBytesPerTurn: 1_472_000,
  // 120 s at 24 kHz s16le.
  maxModelAudioBytesPerTurn: 5_760_000,
  connectsPerIpPerMinute: 10,
  connectsPerDevicePerMinute: 6,
  turnsPerDevicePerMinute: 30,
  authFailuresBeforeBlock: 5,
  authFailureWindowMs: 5 * 60_000,
  authBlockMs: 5 * 60_000,
  geminiSetupTimeoutMs: 15_000,
  geminiIdleCloseMs: 5 * 60_000,
  // Reconnect the Gemini side between turns before the 30-min token expiry.
  geminiMaxAgeMs: 28 * 60_000,
  // Cloud Run's request timeout is 60 min; the device reconnects at ~55 min.
  deviceMaxAgeMs: 57 * 60_000,
  // If Gemini never acknowledges an interruption, stop waiting after this.
  interruptFenceTimeoutMs: 5_000,
  sendHighWaterBytes: 64 * 1024,
} as const;

// Letters (including Romanian diacritics), then letters, spaces, hyphens and
// apostrophes.
const PARTICIPANT_RE = /^\p{L}[\p{L} '\-]*$/u;

export interface ActivityForLimits {
  prompt: string;
  participants: readonly string[];
}

export type ActivityProblem =
  | "prompt_too_long"
  | "too_many_participants"
  | "participant_name_too_long"
  | "participant_name_invalid";

export function checkActivity(activity: ActivityForLimits): ActivityProblem[] {
  const problems: ActivityProblem[] = [];
  if (activity.prompt.length > LIMITS.maxActivityPromptChars) {
    problems.push("prompt_too_long");
  }
  if (activity.participants.length > LIMITS.maxParticipants) {
    problems.push("too_many_participants");
  }
  for (const name of activity.participants) {
    if ([...name].length > LIMITS.maxParticipantNameChars) {
      problems.push("participant_name_too_long");
      break;
    }
  }
  for (const name of activity.participants) {
    if (!PARTICIPANT_RE.test(name)) {
      problems.push("participant_name_invalid");
      break;
    }
  }
  return problems;
}
