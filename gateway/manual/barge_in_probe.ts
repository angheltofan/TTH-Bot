// MANUAL, env-gated: does `activityStart` interrupt a Gemini Live response
// in manual-activity (push-to-talk) mode? (PHASE6_PLAN §5.6)
//
// The Flutter app never does push-to-talk barge-in, so this was unproven.
// The probe:
//   1. obtains credentials (see below);
//   2. opens a Live session with the SAME setup the gateway uses (manual
//      activity detection, Puck voice);
//   3. asks for a long spoken answer;
//   4. after ~1.5 s of audio, sends activityStart + 0.5 s of silence +
//      activityEnd — exactly what the gateway sends on a barge-in turn;
//   5. records whether `interrupted` arrives, how fast, and how much old
//      audio still arrives after the barge-in.
// A control run (--control) lets the answer finish, to show the audio would
// otherwise have continued.
//
// Credentials, first match wins (nothing is printed or written):
//   GATEWAY_MINT_URL + GATEWAY_MINT_SECRET   the gateway's token function
//   GEMINI_API_KEY                           a developer key, LOCAL USE ONLY
//                                            (same as Flutter's --dart-define)
//   TTH_SUPABASE_URL + TTH_PUBLISHABLE_KEY   the Flutter app's `gemini-token`
//
//   GEMINI_API_KEY=... deno task probe:barge-in [--control]

import {
  ACTIVITY_END,
  ACTIVITY_START,
  audioMessage,
  buildSetupMessage,
  GeminiEvent,
  GeminiLink,
  webSocketConnector,
} from "../src/gemini.ts";
import { mintGeminiToken } from "../src/token_client.ts";

const control = Deno.args.includes("--control");

interface Credential {
  value: string;
  param: "access_token" | "key";
  source: string;
}

async function credential(): Promise<Credential> {
  const mintUrl = Deno.env.get("GATEWAY_MINT_URL");
  const secret = Deno.env.get("GATEWAY_MINT_SECRET");
  const publishable = Deno.env.get("TTH_PUBLISHABLE_KEY") ?? "";
  if (mintUrl && secret) {
    return {
      value: await mintGeminiToken({
        fetchFn: fetch,
        functionUrl: mintUrl,
        publishableKey: publishable,
        secret,
        nowMs: Date.now(),
      }),
      param: "access_token",
      source: "gateway-gemini-token",
    };
  }
  const devKey = Deno.env.get("GEMINI_API_KEY");
  if (devKey) return { value: devKey, param: "key", source: "developer key (local only)" };

  const supabaseUrl = Deno.env.get("TTH_SUPABASE_URL");
  if (!supabaseUrl || !publishable) {
    throw new Error(
      "no credentials: set GEMINI_API_KEY, or TTH_SUPABASE_URL + TTH_PUBLISHABLE_KEY, " +
        "or GATEWAY_MINT_URL + GATEWAY_MINT_SECRET",
    );
  }
  const res = await fetch(`${supabaseUrl}/functions/v1/gemini-token`, {
    method: "POST",
    headers: { apikey: publishable, Authorization: `Bearer ${publishable}` },
  });
  if (!res.ok) throw new Error(`gemini-token returned ${res.status}`);
  return { value: (await res.json()).token, param: "access_token", source: "gemini-token" };
}

const SYSTEM = "You are a test voice for an automated check. When asked, tell a long, " +
  "detailed story in English, speaking continuously for at least one minute. " +
  "If you hear silence, briefly say 'I am listening.'";
const REQUEST = "Please tell me a long story about a small robot who learns to play basketball.";

const t0 = performance.now();
const ms = () => Math.round(performance.now() - t0);
const timeline: Array<{ t: number; e: string; bytes?: number }> = [];
let setupDone!: () => void;
const setupComplete = new Promise<void>((r) => (setupDone = r));
let audioBytes = 0;
let bargeAt: number | null = null;
let interruptedAt: number | null = null;
let turnCompleteAt: number | null = null;
let bytesAfterBargeBeforeInterrupt = 0;
let bytesAfterInterrupt = 0;
let closedCode: number | null = null;

function onEvent(e: GeminiEvent) {
  const t = ms();
  switch (e.type) {
    case "setup_complete":
      timeline.push({ t, e: "setup_complete" });
      setupDone();
      return;
    case "audio":
      audioBytes += e.pcm.length;
      if (bargeAt !== null) {
        if (interruptedAt === null) bytesAfterBargeBeforeInterrupt += e.pcm.length;
        else bytesAfterInterrupt += e.pcm.length;
      }
      return;
    case "interrupted":
      if (interruptedAt === null) interruptedAt = t;
      timeline.push({ t, e: "interrupted" });
      return;
    case "turn_complete":
      turnCompleteAt = t;
      timeline.push({ t, e: "turn_complete", bytes: audioBytes });
      return;
    default:
      timeline.push({ t, e: e.type });
  }
}

const sleep = (n: number) => new Promise((r) => setTimeout(r, n));
async function waitFor(cond: () => boolean, timeoutMs: number): Promise<boolean> {
  const end = performance.now() + timeoutMs;
  while (performance.now() < end) {
    if (cond()) return true;
    await sleep(20);
  }
  return cond();
}

const cred = await credential();
const link: GeminiLink = await webSocketConnector(undefined, cred.param)(
  cred.value,
  buildSetupMessage(SYSTEM),
  {
    onEvent,
    onClose: (code) => {
      closedCode = code;
      timeline.push({ t: ms(), e: `closed:${code}` });
    },
  },
);
if (!(await waitFor(() => timeline.some((x) => x.e === "setup_complete"), 15_000))) {
  console.log(JSON.stringify({ error: "setup_complete not received", closedCode, timeline }));
  Deno.exit(1);
}
await setupComplete;

// Ask for the long answer. clientContent first; realtime text as a fallback.
let strategy = "clientContent";
link.send(JSON.stringify({
  clientContent: { turns: [{ role: "user", parts: [{ text: REQUEST }] }], turnComplete: true },
}));
if (!(await waitFor(() => audioBytes > 0, 10_000))) {
  strategy = "realtimeInput.text";
  link.send(ACTIVITY_START);
  link.send(JSON.stringify({ realtimeInput: { text: REQUEST } }));
  link.send(ACTIVITY_END);
  await waitFor(() => audioBytes > 0, 10_000);
}
const firstAudio = audioBytes > 0;

let result: Record<string, unknown>;
if (control) {
  await waitFor(() => turnCompleteAt !== null, 120_000);
  result = {
    mode: "control",
    strategy,
    credential: cred.source,
    responseAudioMs: Math.round(audioBytes / 48),
    turnCompleteAt,
  };
} else {
  // 1.5 s of 24 kHz s16le audio = 72 000 bytes.
  await waitFor(() => audioBytes >= 72_000, 20_000);
  const before = audioBytes;
  bargeAt = ms();
  link.send(ACTIVITY_START);
  const silence = new Uint8Array(640);
  for (let i = 0; i < 25; i++) link.send(audioMessage(silence)); // 0.5 s at 16 kHz
  link.send(ACTIVITY_END);
  timeline.push({ t: bargeAt, e: "barge_in_sent", bytes: before });
  await waitFor(() => interruptedAt !== null, 8_000);
  await sleep(4_000); // watch for a new response to the (silent) barge-in turn
  result = {
    mode: "barge_in",
    strategy,
    credential: cred.source,
    firstAudio,
    audioBeforeBargeMs: Math.round(before / 48),
    interrupted: interruptedAt !== null,
    interruptedAfterMs: interruptedAt === null ? null : interruptedAt - bargeAt,
    oldAudioAfterBargeMs: Math.round(bytesAfterBargeBeforeInterrupt / 48),
    audioAfterInterruptMs: Math.round(bytesAfterInterrupt / 48),
    verdict: interruptedAt !== null ? "INTERRUPTS" : "DOES_NOT_INTERRUPT",
  };
}
link.close();
console.log(JSON.stringify({ ...result, closedCode, timeline }, null, 2));
