// A simulated Gemini upstream for LAN DEVELOPMENT ONLY (Steps 6.2, 6.3).
//
// The device's gates need a gateway that reaches `ready` and answers turns,
// and the real path needs `gateway-gemini-token` deployed — which is gated on
// review (PHASE6_PLAN §4.3). GEMINI_MODE=fake answers locally instead: no
// Gemini, no token mint, no Supabase function, no secret.
//
// ECHO (Step 6.3). The "model speech" is the child's own captured audio: the
// realtimeInput audio between activityStart and activityEnd, converted from
// 16 kHz to 24 kHz mono s16le (src/resample.ts) and sent back after
// `responseDelayMs`, `repeat` times, in audio events of `chunkBytes`, then
// turnComplete. Deterministic and bounded: at most one user turn is held
// (maxUserAudioBytesPerTurn), the conversion is sized exactly, and the
// repetition never exceeds maxModelAudioBytesPerTurn. Audio is never logged.
//
// A new activityStart while a response is still scheduled cancels it and
// answers like Gemini does for an interruption: interrupted, then
// turnComplete.
//
// loadConfig refuses this mode on Cloud Run (K_SERVICE) and behind a proxy
// (TRUST_PROXY=1), so it cannot be deployed by accident.

import { Activity } from "./activities.ts";
import { base64Decode, GeminiConnector, GeminiLink } from "./gemini.ts";
import { LIMITS } from "./limits.ts";
import { resample16kTo24k } from "./resample.ts";

export type FakeSetup = "ok" | "fail" | "never";

export function parseFakeSetup(value: string | undefined): FakeSetup {
  if (value === undefined || value === "" || value === "ok") return "ok";
  if (value === "fail" || value === "never") return value;
  throw new Error("FAKE_GEMINI_SETUP must be ok, fail or never");
}

// A whole number in [min, max]; `fallback` when unset.
export function parseBoundedInt(
  value: string | undefined,
  name: string,
  min: number,
  max: number,
  fallback: number,
): number {
  if (value === undefined || value === "") return fallback;
  if (!/^[0-9]{1,9}$/.test(value)) throw new Error(`${name} must be a whole number`);
  const n = Number(value);
  if (n < min || n > max) throw new Error(`${name} must be between ${min} and ${max}`);
  return n;
}

// Served when GEMINI_MODE=fake runs without Supabase settings.
export const DEV_ACTIVITY: Activity = {
  id: "00000000-0000-4000-8000-000000000000",
  title: "LAN development",
  type: "development",
  prompt: "LAN development session with a simulated Gemini.",
  participants: [],
  interactionMode: "push_to_talk",
  enabled: true,
  sortOrder: 0,
};

export interface FakeEchoOptions {
  // After activityEnd.
  responseDelayMs: number;
  // Replaces responseDelayMs for the FIRST turn to end in this gateway process
  // only. Over 10 s exercises the device's first-response timeout, and the
  // next turn on the same connection is answered normally.
  firstResponseDelayMs?: number;
  // How many times the converted turn is sent (a response longer than the
  // device's 192 000 B ring needs only a few seconds of speech and repeat 2).
  repeat: number;
  // Bytes per Gemini audio event: two 1920 B device frames.
  chunkBytes?: number;
}

export const FAKE_ECHO_DEFAULTS: FakeEchoOptions = { responseDelayMs: 300, repeat: 1 };
export const FAKE_ECHO_MAX_REPEAT = 20;
export const FAKE_RESPONSE_DELAY_MAX_MS = 60_000;

export interface FakeGeminiOptions {
  // ok: setupComplete after the delay · never: no setupComplete (the
  // gateway's 15 s setup timeout fires) · fail: the connection is refused.
  setup: FakeSetup;
  setupDelayMs?: number;
  schedule?: (fn: () => void, ms: number) => void;
  // Absent: no model audio (the Step 6.2 behaviour).
  echo?: FakeEchoOptions;
}

export function fakeGeminiConnector(options: FakeGeminiOptions): GeminiConnector {
  const schedule = options.schedule ??
    ((fn: () => void, ms: number) => {
      setTimeout(fn, ms);
    });
  const delay = options.setupDelayMs ?? 300;
  const echo = options.echo;
  const chunkBytes = echo?.chunkBytes ?? 3840;
  // Once per connector, i.e. once per gateway process.
  let firstResponse = echo?.firstResponseDelayMs !== undefined;
  return (_token, _setup, handlers) => {
    if (options.setup === "fail") return Promise.reject(new Error("fake_gemini_unavailable"));
    let closed = false;

    // One user activity at a time, bounded by the per-turn limit.
    let collecting = false;
    let pieces: Uint8Array[] = [];
    let collected = 0;
    let pending: { cancelled: boolean } | null = null;

    const respond = (user: Uint8Array) => {
      const once = resample16kTo24k(user);
      const maxRepeat = once.length === 0
        ? 0
        : Math.floor(LIMITS.maxModelAudioBytesPerTurn / once.length);
      const repeat = Math.min(echo!.repeat, maxRepeat);
      for (let r = 0; r < repeat && !closed; r++) {
        for (let offset = 0; offset < once.length && !closed; offset += chunkBytes) {
          handlers.onEvent({
            type: "audio",
            pcm: once.subarray(offset, Math.min(once.length, offset + chunkBytes)),
          });
        }
      }
      if (!closed) handlers.onEvent({ type: "turn_complete" });
    };

    const onInput = (text: string) => {
      let input: Record<string, unknown> | undefined;
      try {
        input = (JSON.parse(text) as { realtimeInput?: Record<string, unknown> }).realtimeInput;
      } catch {
        return;
      }
      if (input === undefined || input === null) return;

      if ("activityStart" in input) {
        if (pending !== null) {
          // Interrupted before the response went out.
          pending.cancelled = true;
          pending = null;
          handlers.onEvent({ type: "interrupted" });
          handlers.onEvent({ type: "turn_complete" });
        }
        collecting = true;
        pieces = [];
        collected = 0;
        return;
      }
      if ("audio" in input) {
        if (!collecting) return;
        const audio = input.audio as { data?: unknown };
        if (typeof audio?.data !== "string") return;
        const pcm = base64Decode(audio.data);
        // The gateway already refuses longer turns; never hold more.
        if (collected + pcm.length > LIMITS.maxUserAudioBytesPerTurn) return;
        pieces.push(pcm);
        collected += pcm.length;
        return;
      }
      if ("activityEnd" in input) {
        if (!collecting) return;
        collecting = false;
        const even = collected - (collected % 2);
        const user = new Uint8Array(even);
        let at = 0;
        for (const piece of pieces) {
          const take = Math.min(piece.length, even - at);
          user.set(piece.subarray(0, take), at);
          at += take;
        }
        pieces = [];
        collected = 0;
        const job = { cancelled: false };
        pending = job;
        let wait = echo!.responseDelayMs;
        if (firstResponse) {
          firstResponse = false;
          wait = echo!.firstResponseDelayMs!;
        }
        schedule(() => {
          if (closed || job.cancelled) return;
          pending = null;
          respond(user);
        }, wait);
      }
    };

    const link: GeminiLink = {
      send: (text) => {
        if (!closed && echo !== undefined) onInput(text);
      },
      close: () => {
        closed = true;
        pieces = [];
      },
    };
    if (options.setup === "ok") {
      schedule(() => {
        if (!closed) handlers.onEvent({ type: "setup_complete" });
      }, delay);
    }
    return Promise.resolve(link);
  };
}
