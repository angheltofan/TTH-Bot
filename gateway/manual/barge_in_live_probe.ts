// MANUAL, env-gated: barge-in on the PRODUCTION path (PHASE6_PLAN §5.6.2).
//
// The earlier probe (barge_in_probe.ts) asked for the response with
// `clientContent`, a TEXT turn. That proved Gemini interrupts, but it is not
// what the gateway does. This probe uses ONLY the production sequence:
//
//   setup: automaticActivityDetection { disabled: true }   (manual activity)
//   turn:  activityStart · realtimeInput.audio (16 kHz mono s16le) · activityEnd
//
// It sends user turn 1, waits until Gemini is actively streaming audio, then
// sends user turn 2 the same way — the barge-in.
//
// Attribution (see probe_attribution.ts): from the locally sent turn
// boundaries and the ARRIVAL ORDER of every chunk and event, never from
// counting Gemini lifecycle events. Every chunk and event gets a sequence
// number and a timestamp, so the attribution can be audited.
//
//   cd gateway
//   $env:DENO_TLS_CA_STORE = "system"
//   $env:GEMINI_API_KEY = "<temporary developer key>"
//   D:\deno\deno.exe run --allow-net --allow-env --allow-read --allow-write=manual \
//     manual/barge_in_live_probe.ts [--speech-file path.wav]
//
// Console: the report without per-chunk data. manual/last_live_probe.json:
// the full report including every chunk. The credential is never printed,
// logged or written.

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
import {
  attribute,
  ChunkRecord,
  EventRecord,
  firstEventAfter,
  MODEL_BYTES_PER_MS,
  SentBoundaries,
  Stamp,
} from "./probe_attribution.ts";

const TIMELINE_PATH = "manual/last_live_probe.json";
const FRAME_BYTES = 640; // 20 ms at 16 kHz s16le, exactly what the device sends

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
    throw new Error("no credentials: set GEMINI_API_KEY (local), or the gateway mint settings");
  }
  const res = await fetch(`${supabaseUrl}/functions/v1/gemini-token`, {
    method: "POST",
    headers: { apikey: publishable, Authorization: `Bearer ${publishable}` },
  });
  if (!res.ok) throw new Error(`gemini-token returned ${res.status}`);
  return { value: (await res.json()).token, param: "access_token", source: "gemini-token" };
}

// Answer at length whatever arrives, so the probe does not depend on the
// content of the input audio.
const SYSTEM = "You are a test voice for an automated audio check. Whatever you hear — " +
  "speech, noise or silence — immediately begin telling a long, detailed story in " +
  "English about a small robot learning to play basketball. Speak continuously for at " +
  "least ninety seconds. Never ask questions and never wait for the user.";

function synthesisedUtterance(ms: number): Uint8Array {
  const samples = Math.round((16000 * ms) / 1000);
  const pcm = new Int16Array(samples);
  for (let i = 0; i < samples; i++) {
    const t = i / 16000;
    const envelope = Math.min(1, t * 8) * Math.min(1, (ms / 1000 - t) * 8);
    const wave = Math.sin(2 * Math.PI * 220 * t) + 0.5 * Math.sin(2 * Math.PI * 440 * t) +
      0.25 * Math.sin(2 * Math.PI * 660 * t);
    pcm[i] = Math.max(-32767, Math.min(32767, Math.round(wave * 9000 * envelope)));
  }
  return new Uint8Array(pcm.buffer);
}

async function loadAudio(path: string): Promise<Uint8Array> {
  const bytes = await Deno.readFile(path);
  // Skip a RIFF/WAVE header if present; the payload must already be 16 kHz
  // mono s16le.
  if (bytes.length > 44 && String.fromCharCode(...bytes.subarray(0, 4)) === "RIFF") {
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const channels = view.getUint16(22, true);
    const rate = view.getUint32(24, true);
    const bits = view.getUint16(34, true);
    if (channels !== 1 || rate !== 16000 || bits !== 16) {
      throw new Error(`speech file must be 16 kHz mono 16-bit, got ${rate} Hz ${channels}ch ${bits}bit`);
    }
    let offset = 12;
    while (offset + 8 <= bytes.length) {
      const id = String.fromCharCode(...bytes.subarray(offset, offset + 4));
      const size = view.getUint32(offset + 4, true);
      if (id === "data") return bytes.subarray(offset + 8, offset + 8 + size);
      offset += 8 + size + (size % 2);
    }
    throw new Error("no data chunk in WAV");
  }
  return bytes;
}

const t0 = performance.now();
const sleep = (n: number) => new Promise((r) => setTimeout(r, n));

// --- raw record: every chunk and event, stamped in arrival order ---------------------

let nextSeq = 0;
const stamp = (): Stamp => ({ seq: nextSeq++, atMs: Math.round(performance.now() - t0) });

const chunks: ChunkRecord[] = [];
const events: EventRecord[] = [];
const sent: SentBoundaries = {
  turn1ActivityStart: null,
  turn1ActivityEnd: null,
  turn2ActivityStart: null,
  turn2ActivityEnd: null,
};

let closedCode: number | null = null;
let setupDone!: () => void;
const setupComplete = new Promise<void>((r) => (setupDone = r));

function onEvent(e: GeminiEvent) {
  const s = stamp();
  if (e.type === "audio") {
    chunks.push({ ...s, bytes: e.pcm.length });
    return;
  }
  events.push({ ...s, event: e.type === "error" ? `error:${e.message}` : e.type });
  if (e.type === "setup_complete") setupDone();
}

// --- driving the session ---------------------------------------------------------------

async function waitFor(cond: () => boolean, timeoutMs: number): Promise<boolean> {
  const end = performance.now() + timeoutMs;
  while (performance.now() < end) {
    if (cond()) return true;
    await sleep(20);
  }
  return cond();
}

function bytesAfter(boundary: Stamp | null): number {
  if (boundary === null) return 0;
  return chunks.filter((c) => c.seq > boundary.seq).reduce((s, c) => s + c.bytes, 0);
}

// One user turn, exactly as the gateway sends it: activityStart, 20 ms audio
// frames paced in real time, activityEnd. Records the boundaries it sends.
async function sendManualTurn(link: GeminiLink, pcm: Uint8Array, turn: 1 | 2): Promise<void> {
  const begin = stamp();
  if (turn === 1) sent.turn1ActivityStart = begin;
  else sent.turn2ActivityStart = begin;
  events.push({ ...begin, event: `turn${turn}:activityStart` });
  link.send(ACTIVITY_START);
  for (let offset = 0; offset < pcm.length; offset += FRAME_BYTES) {
    link.send(audioMessage(pcm.subarray(offset, Math.min(pcm.length, offset + FRAME_BYTES))));
    await sleep(20);
  }
  link.send(ACTIVITY_END);
  const end = stamp();
  if (turn === 1) sent.turn1ActivityEnd = end;
  else sent.turn2ActivityEnd = end;
  events.push({ ...end, event: `turn${turn}:activityEnd` });
}

const speechFileIndex = Deno.args.indexOf("--speech-file");
const speech = speechFileIndex >= 0
  ? await loadAudio(Deno.args[speechFileIndex + 1])
  : synthesisedUtterance(1200);
const bargeSpeech = speechFileIndex >= 0
  ? speech.subarray(0, Math.min(speech.length, 16000)) // 0.5 s of the same recording
  : synthesisedUtterance(500);

const cred = await credential();
const link = await webSocketConnector(undefined, cred.param)(
  cred.value,
  buildSetupMessage(SYSTEM),
  {
    onEvent,
    onClose: (code) => {
      closedCode = code;
      events.push({ ...stamp(), event: `closed:${code}` });
    },
  },
);

const finish = async (verdict: string, reason?: string) => {
  const report = {
    probe: "manual realtime activity (production sequence)",
    verdict,
    ...(reason ? { reason } : {}),
    credentialSource: cred.source,
    audioInput: speechFileIndex >= 0 ? "recording" : "synthesised",
    attributionRule:
      "phases from the locally sent turn boundaries, membership by arrival order (seq); " +
      "response2 = audio arriving after turn2 activityEnd",
    ...attribute(chunks, events, sent),
    events,
    closedCode,
  };
  try {
    await Deno.writeTextFile(
      TIMELINE_PATH,
      JSON.stringify({ ...report, audioChunks: chunks }, null, 2),
    );
  } catch {
    // --allow-write not granted: the console output is the record.
  }
  console.log(JSON.stringify({ ...report, audioChunkCount: chunks.length }, null, 2));
  link.close();
  Deno.exit(verdict === "INTERRUPTS" ? 0 : 1);
};

if (!(await waitFor(() => events.some((e) => e.event === "setup_complete"), 15_000))) {
  await finish("INCONCLUSIVE", "setup_complete never arrived");
}
await setupComplete;

// User turn 1 — the production sequence.
await sendManualTurn(link, speech, 1);
if (!(await waitFor(() => bytesAfter(sent.turn1ActivityEnd) > 0, 20_000))) {
  await finish(
    "INCONCLUSIVE",
    "no audio response to the first manual-activity turn; try --speech-file with real speech",
  );
}
// Let it get properly under way: 1.5 s of speech.
await waitFor(() => bytesAfter(sent.turn1ActivityEnd) >= 1500 * MODEL_BYTES_PER_MS, 20_000);
if (events.some((e) => e.event === "turn_complete")) {
  await finish("INCONCLUSIVE", "the first response finished before the barge-in");
}

// User turn 2 — the barge-in, same manual sequence.
await sendManualTurn(link, bargeSpeech, 2);
await waitFor(() => firstEventAfter(events, "interrupted", sent.turn2ActivityStart) !== null, 8_000);
// Give the second response time to start and run a little.
await waitFor(() => bytesAfter(sent.turn2ActivityEnd) > 0, 8_000);
await sleep(2_000);

await finish(
  firstEventAfter(events, "interrupted", sent.turn2ActivityStart) !== null
    ? "INTERRUPTS"
    : "DOES_NOT_INTERRUPT",
);
