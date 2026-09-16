// The cancellation fence, replayed against the timing measured on the real
// Gemini (PHASE6_PLAN §5.6).
//
// What this proves: after the device cancels turn N, every old-response audio
// chunk Gemini is still sending is dropped at the session boundary — it never
// becomes a downstream item, so it can never reach the device, it consumes no
// downstream credit, and it is counted rather than silently discarded. Then
// the new turn's response flows normally.
//
// NOTE ON THE MEASUREMENTS: "old audio after the barge-in" is an AMOUNT of
// audio (bytes / 48 per ms at 24 kHz), not a duration of wall time. On the
// production path 240 ms of audio arrived in the 62 ms before `interrupted`:
// Gemini delivers audio in bursts faster than real time. The replay
// reproduces exactly that: 240 ms of audio spread over 62 ms.
//
// ORDERING: on the production path Gemini sent `interrupted` (3825 ms) and
// then a `turnComplete` closing the interrupted response (3891 ms), both
// BEFORE the barge-in turn's activityEnd (4540 ms). The replay runs that
// observed order, and also the race order in which the barge-in turn's
// turn_end reaches the gateway before that closing turnComplete (a barge-in
// utterance shorter than ~130 ms). Both must deliver turn 2 intact.

import { assert, assertEquals } from "./assert.ts";
import { harness, model, ready, up, userTurn } from "./session_harness.ts";

interface Timing {
  source: string;
  interruptedAfterMs: number; // barge-in activityStart -> interrupted (wall time)
  oldAudioAfterBargeMs: number; // audio received in that window (audio time)
}

const MEASURED: Record<"production" | "clientContent", Timing> = {
  // manual/barge_in_live_probe.ts — manual realtime activity, the gateway's
  // exact sequence. THE measurement that governs the design.
  production: {
    source: "manual realtime activity (production sequence)",
    interruptedAfterMs: 62,
    oldAudioAfterBargeMs: 240,
  },
  // manual/barge_in_probe.ts — first turn via clientContent (text). Diagnostic.
  clientContent: {
    source: "clientContent (diagnostic)",
    interruptedAfterMs: 70,
    oldAudioAfterBargeMs: 280,
  },
};

type Order = "observed" | "race";

const CHUNK_BYTES = 1920; // 40 ms of 24 kHz audio: one device playback slot
const CHUNK_AUDIO_MS = 40;
const TURN1_CHUNKS = 5;
const TURN2_CHUNKS = 3;
const TURN2_FRAMES = 2;

// Replays: turn 1 streaming -> cancel(1) + turn 2 upload -> the measured
// amount of old audio over the measured window -> interrupted -> the
// turnComplete closing response 1 -> turn 2's response.
//
// The device is given EXACTLY enough credit for the audio that should reach
// it (turn 1's chunks before the barge-in plus turn 2's response) and never
// returns any. If a single stale chunk consumed credit, turn 2's response
// could not be delivered in full.
async function replay(timing: Timing, order: Order) {
  const h = harness();
  await ready(h, (TURN1_CHUNKS + TURN2_CHUNKS) * CHUNK_BYTES);

  // Turn 1 is answered; its response is streaming.
  userTurn(h, 1, 2);
  for (let i = 0; i < TURN1_CHUNKS; i++) {
    h.gemini.last.emit(model(CHUNK_BYTES));
    h.timers.advance(CHUNK_AUDIO_MS);
  }
  assertEquals(
    h.device.sent.filter((s) => s.kind === "binary" && s.turn === 1).length,
    TURN1_CHUNKS,
  );
  h.device.take();

  // BARGE-IN: the device cancels turn 1, then starts turn 2 — the order the
  // device's BargeIn sequence and OutboundQueue guarantee on the wire.
  h.session.onText('{"t":"cancel","turn":1}');
  h.session.onText('{"t":"turn_start","turn":2}');
  for (let i = 0; i < TURN2_FRAMES; i++) h.session.onBinary(up(2));
  const turn2End = `{"t":"turn_end","turn":2,"frames":${TURN2_FRAMES},"bytes":${TURN2_FRAMES * 640}}`;
  if (order === "race") h.session.onText(turn2End);

  // The measured AMOUNT of old audio, over the measured wall-time window.
  const staleChunks = Math.round(timing.oldAudioAfterBargeMs / CHUNK_AUDIO_MS);
  assert(staleChunks > 0, "a measurement with no old audio would not exercise the fence");
  const gapMs = timing.interruptedAfterMs / staleChunks;
  for (let i = 0; i < staleChunks; i++) {
    h.gemini.last.emit(model(CHUNK_BYTES));
    h.timers.advance(gapMs);
  }

  // None of it reached the device; all of it was dropped and counted.
  assertEquals(
    h.device.sent.filter((s) => s.kind === "binary").length,
    0,
    `${timing.source} (${order}): old-turn audio reached the device after the cancel`,
  );
  assertEquals(h.session.stats.staleModelFrames, staleChunks);

  // Gemini acknowledges the interruption, then closes the interrupted
  // response with a turnComplete.
  h.gemini.last.emit({ type: "interrupted" });
  h.timers.advance(66);
  h.gemini.last.emit({ type: "turn_complete" });
  if (order === "observed") h.session.onText(turn2End);

  // Turn 2's response.
  for (let i = 0; i < TURN2_CHUNKS; i++) {
    h.gemini.last.emit(model(CHUNK_BYTES));
    h.timers.advance(CHUNK_AUDIO_MS);
  }
  h.gemini.last.emit({ type: "turn_complete" });

  return { h, staleChunks };
}

const TURN2_DELIVERED = [
  "speech_start",
  "audio:2:1920",
  "audio:2:1920",
  "audio:2:1920",
  "turn_complete",
];

Deno.test("fence (production, observed order): 240 ms of old audio never reaches the device", async () => {
  const { h, staleChunks } = await replay(MEASURED.production, "observed");
  assertEquals(staleChunks, 6); // 240 ms of 24 kHz audio in 40 ms chunks
  assert(h.logs.some((l) => l.includes("interrupt_acknowledged")));
});

Deno.test("fence (production, observed order): the old audio consumes no downstream credit", async () => {
  const { h, staleChunks } = await replay(MEASURED.production, "observed");
  // Credit covered exactly turn 1's pre-barge chunks plus turn 2's response;
  // turn 2 arriving IN FULL proves the dropped chunks spent none of it.
  assertEquals(h.device.take(), TURN2_DELIVERED);
  assertEquals(h.session.stats.staleModelFrames, staleChunks);
  assertEquals(h.session.stats.closedInterruptedResponses, 1);
});

// The race the first version of the gateway got wrong: the turnComplete that
// closes the interrupted response arrives AFTER turn 2's turn_end. It was
// taken as turn 2's end, and turn 2's real audio was then dropped as stale.
Deno.test("fence (production, race order): the closing turnComplete does not end turn 2", async () => {
  const { h, staleChunks } = await replay(MEASURED.production, "race");
  assertEquals(h.device.take(), TURN2_DELIVERED);
  assertEquals(h.session.stats.staleModelFrames, staleChunks); // turn 2 audio not dropped
  assertEquals(h.session.stats.closedInterruptedResponses, 1);
  assert(h.logs.some((l) => l.includes("interrupted_response_closed")));
});

Deno.test("fence (clientContent diagnostic timing): both orders hold", async () => {
  for (const order of ["observed", "race"] as const) {
    const { h, staleChunks } = await replay(MEASURED.clientContent, order);
    assertEquals(staleChunks, 7);
    assertEquals(h.device.take(), TURN2_DELIVERED, order);
  }
});

Deno.test("fence: a far harsher timing than measured still holds, in both orders", async () => {
  // 1.2 s of old audio over 1.5 s before interrupted — well inside the 5 s
  // fence timeout.
  for (const order of ["observed", "race"] as const) {
    const { h } = await replay(
      { source: "stress", interruptedAfterMs: 1500, oldAudioAfterBargeMs: 1200 },
      order,
    );
    assertEquals(h.session.stats.staleModelFrames, 30, order);
    assertEquals(h.device.take(), TURN2_DELIVERED, order);
  }
});

// If Gemini never sends a closing turnComplete, new-response audio clears the
// expectation, so turn 2's own turnComplete still ends turn 2.
Deno.test("fence: without a closing turnComplete, turn 2's own turnComplete still ends it", async () => {
  const h = harness();
  await ready(h);
  userTurn(h, 1, 2);
  h.gemini.last.emit(model(CHUNK_BYTES));
  h.session.onText('{"t":"cancel","turn":1}');
  userTurn(h, 2, 2);
  h.gemini.last.emit({ type: "interrupted" });
  h.device.take();

  h.gemini.last.emit(model(CHUNK_BYTES)); // turn 2's response begins
  h.gemini.last.emit({ type: "turn_complete" });
  assertEquals(h.device.take(), ["speech_start", "audio:2:1920", "turn_complete"]);
  assertEquals(h.session.stats.closedInterruptedResponses, 0);
});
