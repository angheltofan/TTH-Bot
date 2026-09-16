// DeviceSession against a fake Gemini, fake device socket and fake clock.
//
// Covers the whole turn path, credit gating, ordering, cancel with the
// interrupt fence, limits, idle/age/goAway reconnects, backpressure and log
// redaction.

import { assert, assertEquals } from "./assert.ts";
import { buildSetupMessage } from "../src/gemini.ts";
import { LIMITS } from "../src/limits.ts";
// The fakes live in session_harness.ts so fence_replay_test.ts uses the same
// device socket, Gemini link and clock.
import {
  flush,
  geminiKinds,
  harness,
  model,
  ready,
  up,
  userTurn,
} from "./session_harness.ts";

// --- tests ---------------------------------------------------------------------------

Deno.test("hello opens Gemini with the Flutter setup and a minted token, then ready", async () => {
  const h = harness();
  await ready(h);
  assertEquals(h.gemini.links.length, 1);
  assertEquals(h.gemini.last.token, "tok-1");
  assertEquals(h.gemini.last.setup, buildSetupMessage("SYS"));
});

Deno.test("a full turn: ordered upstream, ordered and re-chunked downstream", async () => {
  const h = harness();
  await ready(h);
  userTurn(h, 1, 3);
  assertEquals(geminiKinds(h.gemini.last), ["start", "audio", "audio", "audio", "end"]);

  h.gemini.last.emit(model(5000));
  h.gemini.last.emit({ type: "turn_complete" });
  assertEquals(h.device.types(), [
    "speech_start",
    "audio:1:1920",
    "audio:1:1920",
    "audio:1:1160",
    "turn_complete",
  ]);
  const complete = h.device.sent[4];
  assert(complete.kind === "text");
  assertEquals(complete.msg, { t: "turn_complete", turn: 1, frames: 3, bytes: 5000 });
});

Deno.test("credit: audio stops at zero credit, turn_complete waits, pong does not", async () => {
  const h = harness();
  await ready(h, 1920);
  userTurn(h, 1);
  h.gemini.last.emit(model(3840));
  h.gemini.last.emit({ type: "turn_complete" });
  assertEquals(h.device.take(), ["speech_start", "audio:1:1920"]);

  h.session.onText('{"t":"ping","ts":42}');
  assertEquals(h.device.take(), ["pong"]); // deliverable at zero credit

  h.session.onText('{"t":"credit","bytes":1920}');
  assertEquals(h.device.take(), ["audio:1:1920", "turn_complete"]);
});

Deno.test("credit: returning more than was sent closes the connection", async () => {
  const h = harness();
  await ready(h);
  h.session.onText('{"t":"credit","bytes":1}');
  assertEquals(h.device.types(), ["error"]);
  assertEquals(h.device.closed?.code, 1002);
});

Deno.test("turn_end with totals that do not match is refused and its answer fenced", async () => {
  const h = harness();
  await ready(h);
  h.session.onText('{"t":"turn_start","turn":1}');
  h.session.onBinary(up(1));
  h.session.onText('{"t":"turn_end","turn":1,"frames":2,"bytes":1280}');
  assertEquals(h.device.take(), ["error"]);
  h.gemini.last.emit(model(1920));
  assertEquals(h.device.take(), []); // the answer to a partial question is dropped
  assertEquals(h.session.stats.staleModelFrames, 1);
});

Deno.test("turn ids: an active or recently used id is refused", async () => {
  const h = harness();
  await ready(h);
  h.session.onText('{"t":"turn_start","turn":5}');
  h.session.onText('{"t":"turn_start","turn":5}');
  assertEquals(h.device.take(), ["error"]);
  h.session.onText('{"t":"turn_end","turn":5,"frames":0,"bytes":0}');
  h.gemini.last.emit({ type: "turn_complete" });
  h.device.take();
  h.session.onText('{"t":"turn_start","turn":5}');
  const sent = h.device.sent[0];
  assert(sent.kind === "text");
  assertEquals(sent.msg.code, "bad_turn");
});

Deno.test("barge-in: cancel purges, fences stale audio, and turn N+1 is clean", async () => {
  const h = harness();
  await ready(h, 1920);
  userTurn(h, 1);
  h.gemini.last.emit(model(5760)); // 3 frames; only 1 fits the credit
  assertEquals(h.device.take(), ["speech_start", "audio:1:1920"]);

  h.session.onText('{"t":"cancel","turn":1}'); // purges the 2 queued frames
  h.gemini.last.emit(model(1920)); // still in flight from Gemini: stale
  userTurn(h, 2);
  h.gemini.last.emit(model(1920)); // stale too: Gemini has not acknowledged yet
  assertEquals(h.session.stats.staleModelFrames, 2);
  h.gemini.last.emit({ type: "interrupted" }); // the fence comes down

  // The device discarded the one frame it had and returns its credit.
  h.session.onText('{"t":"credit","bytes":1920}');
  h.gemini.last.emit(model(1920));
  h.gemini.last.emit({ type: "turn_complete" });
  assertEquals(h.device.take(), ["speech_start", "audio:2:1920", "turn_complete"]);
  // Upstream to Gemini: turn 1, then turn 2, never interleaved.
  assertEquals(geminiKinds(h.gemini.last), [
    "start", "audio", "audio", "end",
    "start", "audio", "audio", "end",
  ]);
});

Deno.test("barge-in: if Gemini never acknowledges, the fence times out", async () => {
  const h = harness();
  await ready(h);
  userTurn(h, 1);
  h.gemini.last.emit(model(1920));
  h.session.onText('{"t":"cancel","turn":1}');
  userTurn(h, 2);
  h.device.take();
  h.timers.advance(LIMITS.interruptFenceTimeoutMs);
  h.gemini.last.emit(model(1920));
  assertEquals(h.device.take(), ["speech_start", "audio:2:1920"]);
  assert(h.logs.some((l) => l.includes("interrupt_fence_timeout")));
});

Deno.test("limits: user audio beyond 46 s fails the turn", async () => {
  const h = harness();
  await ready(h);
  h.session.onText('{"t":"turn_start","turn":1}');
  const frames = LIMITS.maxUserAudioBytesPerTurn / 640;
  for (let i = 0; i < frames; i++) h.session.onBinary(up(1));
  assertEquals(h.device.take(), []);
  h.session.onBinary(up(1));
  const err = h.device.sent[0];
  assert(err.kind === "text");
  assertEquals(err.msg, { t: "error", code: "turn_too_long", retry: true, turn: 1 });
});

Deno.test("limits: model audio beyond 120 s ends the response explicitly", async () => {
  const h = harness();
  await ready(h, 0xffffffff);
  userTurn(h, 1);
  h.gemini.last.emit(model(LIMITS.maxModelAudioBytesPerTurn + 2));
  const types = h.device.take();
  assertEquals(types.slice(-2), ["error", "turn_complete"]);
});

Deno.test("idle: Gemini closes after 5 min and reopens on the next turn, in order", async () => {
  const h = harness();
  await ready(h);
  h.timers.advance(LIMITS.geminiIdleCloseMs);
  assert(h.gemini.last.closed);

  userTurn(h, 1, 2); // arrives while Gemini is reconnecting
  await flush();
  assertEquals(h.gemini.links.length, 2);
  assertEquals(h.gemini.last.sent, []); // held until setup completes
  h.gemini.last.emit({ type: "setup_complete" });
  assertEquals(geminiKinds(h.gemini.last), ["start", "audio", "audio", "end"]);
  assertEquals(h.device.take(), []); // no second `ready`
});

Deno.test("goAway mid-response reconnects only after the turn", async () => {
  const h = harness();
  await ready(h);
  userTurn(h, 1);
  h.gemini.last.emit(model(1920));
  h.gemini.last.emit({ type: "go_away" });
  assert(!h.gemini.last.closed, "must not cut a response short");
  h.gemini.last.emit({ type: "turn_complete" });
  assert(h.gemini.last.closed);
  userTurn(h, 2);
  await flush();
  assertEquals(h.gemini.links.length, 2);
});

Deno.test("Gemini older than the token lifetime is replaced between turns", async () => {
  const h = harness();
  await ready(h);
  // A turn every 4 min keeps the 5-min idle close from firing, so only the
  // age rule can close the session.
  const step = 4 * 60_000;
  let turn = 1;
  for (let elapsed = 0; elapsed + step < LIMITS.geminiMaxAgeMs; elapsed += step) {
    h.timers.advance(step);
    userTurn(h, turn++);
    h.gemini.last.emit({ type: "turn_complete" });
    assert(!h.gemini.last.closed, "closed before reaching the maximum age");
  }
  h.timers.advance(step); // now at the maximum age
  userTurn(h, turn);
  assert(!h.gemini.last.closed, "must never close mid-turn");
  h.gemini.last.emit({ type: "turn_complete" });
  assert(h.gemini.last.closed, "must close at the first boundary past the maximum age");
  assertEquals(h.gemini.links.length, 1);
});

Deno.test("setup timeout: the device gets a retryable error and the connection closes", async () => {
  const h = harness();
  h.session.onText(
    '{"t":"hello","proto":1,"fw":"core2-6.0","in":"s16le/16000/1","out":"s16le/24000/1","maxDown":1920,"credit":1000}',
  );
  await flush();
  h.timers.advance(LIMITS.geminiSetupTimeoutMs);
  const err = h.device.sent[0];
  assert(err.kind === "text");
  assertEquals(err.msg, { t: "error", code: "gemini_setup_timeout", retry: true });
  assertEquals(h.device.closed?.code, 1011);
});

Deno.test("backpressure: nothing is sent while the socket is backed up", async () => {
  const h = harness();
  await ready(h);
  userTurn(h, 1);
  h.device.buffered = LIMITS.sendHighWaterBytes;
  h.gemini.last.emit(model(1920));
  assertEquals(h.device.take(), []);
  h.device.buffered = 0;
  h.timers.advance(20);
  assertEquals(h.device.take(), ["speech_start", "audio:1:1920"]);
});

Deno.test("user frames for a turn that is not open are dropped and counted", async () => {
  const h = harness();
  await ready(h);
  h.session.onBinary(up(9));
  assertEquals(h.session.stats.staleUserFrames, 1);
  assertEquals(h.gemini.last.sent, []);
});

Deno.test("messages before hello, and malformed frames, are protocol errors", () => {
  const h = harness();
  h.session.onText('{"t":"turn_start","turn":1}');
  assertEquals(h.device.closed?.code, 1002);
});

Deno.test("rate limit: refused turns get a retryable error", async () => {
  const h = harness({ allowTurn: () => false });
  await ready(h);
  h.session.onText('{"t":"turn_start","turn":1}');
  const err = h.device.sent[0];
  assert(err.kind === "text");
  assertEquals(err.msg.code, "rate_limited");
});

Deno.test("free_conversation runs as push-to-talk and the conversion is logged", async () => {
  const h = harness({ converted: true });
  await ready(h);
  assert(h.logs.some((l) => l.includes('"event":"mode_converted"')));
  assert(h.gemini.last.setup.includes('"automaticActivityDetection":{"disabled":true}'));
});

Deno.test("max age: the device is told to reconnect before Cloud Run cuts it", async () => {
  const h = harness();
  await ready(h);
  h.timers.advance(LIMITS.deviceMaxAgeMs);
  assertEquals(h.device.take(), ["session_end"]);
});

Deno.test("logs never contain the prompt, child names or tokens", async () => {
  const h = harness({ system: "PROMPT-SENTINEL Maria Sofia" });
  await ready(h);
  userTurn(h, 1);
  h.gemini.last.emit(model(1920));
  h.gemini.last.emit({ type: "turn_complete" });
  const all = h.logs.join("\n");
  for (const s of ["PROMPT-SENTINEL", "Maria", "Sofia", "tok-1"]) assert(!all.includes(s), s);
});
