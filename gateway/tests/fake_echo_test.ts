// Step 6.3 LAN fake mode: the echo response, its bounds, interruption, the
// credit test controls, configuration guard rails, and no audio in the logs.

import { assert, assertEquals } from "./assert.ts";
import { DEV_ACTIVITY, fakeGeminiConnector, FakeEchoOptions } from "../src/fake_gemini.ts";
import { ACTIVITY_END, ACTIVITY_START, audioMessage, GeminiEvent } from "../src/gemini.ts";
import { LIMITS } from "../src/limits.ts";
import { createLogger } from "../src/log.ts";
import { resample16kTo24k } from "../src/resample.ts";
import { loadConfig } from "../src/server.ts";
import { DeviceSession, SessionTestControls } from "../src/session.ts";
import { FakeDevice, FakeTimers, flush, up } from "./session_harness.ts";

function throws(fn: () => unknown): boolean {
  try {
    fn();
    return false;
  } catch {
    return true;
  }
}

function ramp(bytes: number, start = 0): Uint8Array {
  const out = new Uint8Array(bytes);
  const view = new DataView(out.buffer);
  for (let i = 0; i < bytes / 2; i++) view.setInt16(i * 2, ((start + i) * 37) % 20000, true);
  return out;
}

async function connector(echo: FakeEchoOptions) {
  const scheduled: Array<{ fn: () => void; ms: number }> = [];
  const events: GeminiEvent[] = [];
  const link = await fakeGeminiConnector({
    setup: "never",
    schedule: (fn, ms) => {
      scheduled.push({ fn, ms });
    },
    echo,
  })("t", "{}", { onEvent: (e) => events.push(e), onClose: () => {} });
  const run = () => scheduled.splice(0).forEach((s) => s.fn());
  return { link, events, scheduled, run };
}

function audioOf(events: GeminiEvent[]): Uint8Array {
  const parts = events.filter((e) => e.type === "audio") as Array<{ type: "audio"; pcm: Uint8Array }>;
  const total = parts.reduce((n, p) => n + p.pcm.length, 0);
  const out = new Uint8Array(total);
  let at = 0;
  for (const p of parts) {
    out.set(p.pcm, at);
    at += p.pcm.length;
  }
  return out;
}

// --- configuration ------------------------------------------------------------------

Deno.test("fake echo settings: defaults, bounds, and refused outside fake mode", () => {
  const base: Record<string, string> = { GEMINI_MODE: "fake", TTH_DEVICES: "{}" };
  const cfg = loadConfig((n) => base[n]);
  assertEquals(cfg.fakeEcho, { responseDelayMs: 300, repeat: 1 });
  assertEquals(cfg.fakeCreditHoldMs, 0);
  assertEquals(cfg.fakeOverCreditFrames, 0);

  const set = (extra: Record<string, string>) => loadConfig((n) => ({ ...base, ...extra })[n]);
  const tuned = set({
    FAKE_RESPONSE_DELAY_MS: "11000",
    FAKE_ECHO_REPEAT: "3",
    FAKE_CREDIT_HOLD_MS: "2000",
    FAKE_VIOLATE_CREDIT: "1",
  });
  assertEquals(tuned.fakeEcho, { responseDelayMs: 11000, repeat: 3 });
  assertEquals(tuned.fakeCreditHoldMs, 2000);
  assertEquals(tuned.fakeOverCreditFrames, 1);

  assert(throws(() => set({ FAKE_ECHO_REPEAT: "0" })), "repeat below 1");
  assert(throws(() => set({ FAKE_ECHO_REPEAT: "21" })), "repeat above 20");
  assert(throws(() => set({ FAKE_RESPONSE_DELAY_MS: "60001" })), "delay above 60 s");
  assert(throws(() => set({ FAKE_VIOLATE_CREDIT: "2" })), "violate is 0 or 1");
  assert(throws(() => set({ FAKE_CREDIT_HOLD_MS: "-5" })), "negative hold");
  assert(throws(() => set({ FAKE_ECHO_REPEAT: "2.5" })), "not a whole number");

  // Live mode (which needs its secrets) refuses every fake-only setting.
  const live: Record<string, string> = {
    TTH_DEVICES: "{}",
    SUPABASE_URL: "https://x.supabase.co",
    SUPABASE_PUBLISHABLE_KEY: "pk",
    GATEWAY_MINT_SECRET: "s".repeat(32),
  };
  assertEquals(loadConfig((n) => live[n]).geminiMode, "live");
  for (const name of ["FAKE_ECHO_REPEAT", "FAKE_CREDIT_HOLD_MS", "FAKE_VIOLATE_CREDIT"]) {
    assert(throws(() => loadConfig((n) => ({ ...live, [name]: "1" })[n])), name);
  }
});

// --- the echo -------------------------------------------------------------------------

Deno.test("the echo is the user turn converted to 24 kHz, after the delay, then turn_complete", async () => {
  const c = await connector({ responseDelayMs: 700, repeat: 1 });
  const user = [ramp(640, 0), ramp(640, 320), ramp(202, 640)]; // two frames and a partial tail
  c.link.send(ACTIVITY_START);
  for (const pcm of user) c.link.send(audioMessage(pcm));
  c.link.send(ACTIVITY_END);
  assertEquals(c.events.length, 0);
  assertEquals(c.scheduled.length, 1);
  assertEquals(c.scheduled[0].ms, 700);
  c.run();

  const concat = new Uint8Array(1482);
  concat.set(user[0], 0);
  concat.set(user[1], 640);
  concat.set(user[2], 1280);
  const expected = resample16kTo24k(concat);
  assertEquals(audioOf(c.events), expected);
  assertEquals(expected.length, Math.floor((741 * 3) / 2) * 2);
  assertEquals(c.events[c.events.length - 1].type, "turn_complete");
  // Two device frames per event, the last one partial.
  for (const e of c.events.slice(0, -2)) if (e.type === "audio") assertEquals(e.pcm.length, 3840);
});

Deno.test("FAKE_FIRST_RESPONSE_DELAY_MS delays only the first response of the process", async () => {
  const c = await connector({ responseDelayMs: 300, repeat: 1, firstResponseDelayMs: 11000 });
  for (let turn = 0; turn < 3; turn++) {
    c.link.send(ACTIVITY_START);
    c.link.send(audioMessage(ramp(640)));
    c.link.send(ACTIVITY_END);
  }
  assertEquals(c.scheduled.map((s) => s.ms), [11000, 300, 300]);

  const base: Record<string, string> = { GEMINI_MODE: "fake", TTH_DEVICES: "{}" };
  const cfg = loadConfig((n) => ({ ...base, FAKE_FIRST_RESPONSE_DELAY_MS: "11000" })[n]);
  assertEquals(cfg.fakeEcho?.firstResponseDelayMs, 11000);
  assertEquals(loadConfig((n) => base[n]).fakeEcho?.firstResponseDelayMs, undefined);
});

Deno.test("repeat sends the echo n times; an empty turn completes with no audio", async () => {
  const c = await connector({ responseDelayMs: 0, repeat: 3 });
  c.link.send(ACTIVITY_START);
  c.link.send(audioMessage(ramp(6400)));
  c.link.send(ACTIVITY_END);
  c.run();
  const once = resample16kTo24k(ramp(6400));
  const all = audioOf(c.events);
  assertEquals(all.length, once.length * 3);
  assertEquals(all.subarray(once.length * 2), once);

  const empty = await connector({ responseDelayMs: 0, repeat: 5 });
  empty.link.send(ACTIVITY_START);
  empty.link.send(ACTIVITY_END);
  empty.run();
  assertEquals(empty.events.map((e) => e.type), ["turn_complete"]);
});

Deno.test("the echo is bounded by the per-turn limits", async () => {
  const c = await connector({ responseDelayMs: 0, repeat: 20 });
  c.link.send(ACTIVITY_START);
  const frame = audioMessage(new Uint8Array(64000));
  for (let i = 0; i < 30; i++) c.link.send(frame); // 1.92 MB offered, 1.472 MB allowed
  c.link.send(ACTIVITY_END);
  c.run();
  const total = audioOf(c.events).length;
  assert(total <= LIMITS.maxModelAudioBytesPerTurn, `model audio ${total} within the limit`);
  // 23 whole 64 KB frames fit the 1 472 000 B user limit exactly.
  const once = resample16kTo24k(new Uint8Array(1_472_000)).length;
  assertEquals(total % once, 0);
  assertEquals(total / once, Math.floor(LIMITS.maxModelAudioBytesPerTurn / once));
});

Deno.test("a new activity before the response interrupts it; audio outside an activity is ignored", async () => {
  const c = await connector({ responseDelayMs: 11000, repeat: 1 });
  c.link.send(audioMessage(ramp(640))); // no activity: ignored
  c.link.send(ACTIVITY_START);
  c.link.send(audioMessage(ramp(640)));
  c.link.send(ACTIVITY_END);
  c.link.send(ACTIVITY_START);
  assertEquals(c.events.map((e) => e.type), ["interrupted", "turn_complete"]);
  c.link.send(audioMessage(ramp(1280)));
  c.link.send(ACTIVITY_END);
  c.run(); // the first response was cancelled; only the second is sent
  const audio = audioOf(c.events);
  assertEquals(audio, resample16kTo24k(ramp(1280)));

  const closed = await connector({ responseDelayMs: 0, repeat: 1 });
  closed.link.send(ACTIVITY_START);
  closed.link.send(audioMessage(ramp(640)));
  closed.link.send(ACTIVITY_END);
  closed.link.close();
  closed.run();
  assertEquals(closed.events.length, 0);
});

// --- through a device session ----------------------------------------------------------

async function echoSession(credit: number, controls?: SessionTestControls, delayMs = 100) {
  const timers = new FakeTimers();
  const device = new FakeDevice();
  const logs: string[] = [];
  const session = new DeviceSession({
    deviceId: "core2-01",
    sessionId: "s-1",
    resolved: { activity: DEV_ACTIVITY, convertedFromFreeConversation: false },
    systemInstruction: "SYS",
    device,
    mintToken: () => Promise.resolve("fake-gemini-token"),
    connectGemini: fakeGeminiConnector({
      setup: "ok",
      setupDelayMs: 0,
      schedule: (fn, ms) => {
        timers.set(fn, ms);
      },
      echo: { responseDelayMs: delayMs, repeat: 1 },
    }),
    timers,
    log: createLogger((line) => logs.push(line), () => "T"),
    testControls: controls,
  });
  session.onText(
    `{"t":"hello","proto":1,"fw":"core2-6.3","in":"s16le/16000/1","out":"s16le/24000/1","maxDown":1920,"credit":${credit}}`,
  );
  await flush();
  await flush();
  timers.advance(0);
  assertEquals(device.take(), ["ready"]);
  return { timers, device, logs, session };
}

// 10 frames of 640 B = 3200 samples -> 4800 samples = 9600 B = five 1920 B frames.
function userTurn(session: DeviceSession, turn: number, frames = 10) {
  session.onText(`{"t":"turn_start","turn":${turn}}`);
  for (let i = 0; i < frames; i++) session.onBinary(up(turn, 640, 3));
  session.onText(`{"t":"turn_end","turn":${turn},"frames":${frames},"bytes":${frames * 640}}`);
}

const F = "audio:1:1920";

Deno.test("faster than real time until zero credit, exactly; resumes as credit returns; completes", async () => {
  const h = await echoSession(3840);
  userTurn(h.session, 1);
  h.timers.advance(100);
  assertEquals(h.device.take(), ["speech_start", F, F]); // stopped at exactly 0
  h.session.onText(`{"t":"credit","bytes":3840}`);
  assertEquals(h.device.take(), [F, F]);
  h.session.onText(`{"t":"credit","bytes":1920}`);
  const last = h.device.sent;
  assertEquals(h.device.take(), [F, "turn_complete"]);
  const done = last[last.length - 1];
  assert(done.kind === "text" && done.msg.frames === 5 && done.msg.bytes === 9600, "totals");
});

Deno.test("FAKE_CREDIT_HOLD_MS keeps the device at zero credit, then releases", async () => {
  const h = await echoSession(3840, { creditHoldMs: 500 });
  userTurn(h.session, 1);
  h.timers.advance(100);
  assertEquals(h.device.take(), ["speech_start", F, F]);
  h.session.onText(`{"t":"credit","bytes":1920}`);
  h.session.onText(`{"t":"credit","bytes":1920}`);
  // Control messages still flow at zero audio credit.
  h.session.onText(`{"t":"ping","ts":7}`);
  assertEquals(h.device.take(), ["pong"]);
  h.timers.advance(499);
  assertEquals(h.device.take(), []);
  h.timers.advance(1);
  assertEquals(h.device.take(), [F, F]);
  assert(h.logs.some((l) => l.includes("test_credit_hold")), "the hold is logged");
});

Deno.test("FAKE_VIOLATE_CREDIT sends one frame beyond credit, once per process", async () => {
  const budget = { remaining: 1 };
  const h = await echoSession(3840, { overCreditFrames: budget });
  userTurn(h.session, 1);
  h.timers.advance(100);
  assertEquals(h.device.take(), ["speech_start", F, F, F]); // the third is over credit
  assertEquals(budget.remaining, 0);
  assert(h.logs.some((l) => l.includes("test_over_credit_frame")), "logged");

  // A second session (the reconnect) shares the spent budget: clean.
  const again = await echoSession(3840, { overCreditFrames: budget });
  userTurn(again.session, 1);
  again.timers.advance(100);
  assertEquals(again.device.take(), ["speech_start", F, F]);
});

Deno.test("an over-delayed first response is still delivered when it comes", async () => {
  const h = await echoSession(192000, undefined, 11000);
  userTurn(h.session, 1);
  h.timers.advance(10999);
  assertEquals(h.device.take(), []);
  h.timers.advance(1);
  assertEquals(h.device.take(), ["speech_start", F, F, F, F, F, "turn_complete"]);
});

Deno.test("cancelling a completed response still queued behind credit purges it", async () => {
  const h = await echoSession(3840);
  userTurn(h.session, 1);
  h.timers.advance(100);
  assertEquals(h.device.take(), ["speech_start", F, F]);
  // Barge-in: the device cancels, returns what it discarded, starts turn 2.
  h.session.onText(`{"t":"cancel","turn":1}`);
  h.session.onText(`{"t":"credit","bytes":3840}`);
  assertEquals(h.device.take(), []);
  assert(h.logs.some((l) => l.includes("turn_cancelled")), "purge logged");
  userTurn(h.session, 2, 2);
  h.timers.advance(100);
  assertEquals(h.device.take(), ["speech_start", "audio:2:1920", "turn_complete"]);
});

Deno.test("no audio or base64 payload ever reaches the logs", async () => {
  const h = await echoSession(3840, { creditHoldMs: 200, overCreditFrames: { remaining: 1 } });
  userTurn(h.session, 1);
  h.timers.advance(100);
  h.session.onText(`{"t":"credit","bytes":3840}`);
  h.timers.advance(200);
  h.session.onText(`{"t":"cancel","turn":1}`);
  const payload = JSON.parse(audioMessage(up(1, 640, 3).subarray(8))).realtimeInput.audio.data;
  for (const line of h.logs) {
    assert(!line.includes(payload), "no base64 audio");
    assert(!line.includes("AwMD"), "no PCM fragments");
    assert(line.length < 400, "log lines stay small");
  }
});
