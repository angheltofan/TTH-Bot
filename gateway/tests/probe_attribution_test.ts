// The production barge-in probe's attribution, replayed against the recorded
// production run (PHASE6_PLAN §5.6.2).
//
// The run's first report said `secondTurnGotResponse: false` while
// "generation 3" held 33,161 ms of audio: the probe had advanced a generation
// on `interrupted` AND on the `turnComplete` that closes the interrupted
// response. This test rebuilds that run from its recorded timestamps and
// aggregates and checks the corrected attribution produces a self-consistent
// report, while keeping the three interruption measurements unchanged.
//
// Recorded (manual/last_live_probe.json, 2026-09-14):
//   751 setup_complete · 770 turn1 activityStart · 2547 turn1 activityEnd
//   response-1 audio 3340..3825, 1851 ms total (1611 ms before the barge-in)
//   3763 turn2 activityStart · 3825 interrupted · 3891 turn_complete
//   4540 turn2 activityEnd · response-2 audio 5553..14553, 33,161 ms
// The last response-1 chunk and `interrupted` share the 3825 ms timestamp;
// the chunk arrived first (it was counted as old audio at the time).

import { assertEquals } from "./assert.ts";
import {
  attribute,
  ChunkRecord,
  EventRecord,
  MODEL_BYTES_PER_MS,
  SentBoundaries,
} from "../manual/probe_attribution.ts";

type Item =
  | { kind: "event"; atMs: number; event: string }
  | { kind: "chunk"; atMs: number; bytes: number };

// Splits `audioMs` of 24 kHz audio into 40 ms chunks spread evenly over
// [fromMs, toMs]; the final chunk carries the remainder.
function audioSpan(audioMs: number, fromMs: number, toMs: number): Item[] {
  const total = audioMs * MODEL_BYTES_PER_MS;
  const full = 1920;
  const out: Item[] = [];
  const count = Math.ceil(total / full);
  for (let i = 0; i < count; i++) {
    const bytes = i < count - 1 ? full : total - full * (count - 1);
    const atMs = count === 1 ? fromMs : Math.round(fromMs + ((toMs - fromMs) * i) / (count - 1));
    out.push({ kind: "chunk", atMs, bytes });
  }
  return out;
}

function record(items: Item[]) {
  const chunks: ChunkRecord[] = [];
  const events: EventRecord[] = [];
  const sent: SentBoundaries = {
    turn1ActivityStart: null,
    turn1ActivityEnd: null,
    turn2ActivityStart: null,
    turn2ActivityEnd: null,
  };
  items.forEach((item, seq) => {
    if (item.kind === "chunk") {
      chunks.push({ seq, atMs: item.atMs, bytes: item.bytes });
      return;
    }
    const e = { seq, atMs: item.atMs, event: item.event };
    events.push(e);
    if (item.event === "turn1:activityStart") sent.turn1ActivityStart = e;
    if (item.event === "turn1:activityEnd") sent.turn1ActivityEnd = e;
    if (item.event === "turn2:activityStart") sent.turn2ActivityStart = e;
    if (item.event === "turn2:activityEnd") sent.turn2ActivityEnd = e;
  });
  return { chunks, events, sent };
}

// The recorded production run, in arrival order.
function productionRun(): Item[] {
  return [
    { kind: "event", atMs: 751, event: "setup_complete" },
    { kind: "event", atMs: 770, event: "turn1:activityStart" },
    { kind: "event", atMs: 2547, event: "turn1:activityEnd" },
    ...audioSpan(1611, 3340, 3762), // response 1, before the barge-in
    { kind: "event", atMs: 3763, event: "turn2:activityStart" },
    ...audioSpan(240, 3764, 3825), // old audio after the barge-in (last chunk at 3825)
    { kind: "event", atMs: 3825, event: "interrupted" }, // same ms, arrived after
    { kind: "event", atMs: 3891, event: "turn_complete" }, // closes response 1
    { kind: "event", atMs: 4540, event: "turn2:activityEnd" },
    ...audioSpan(33161, 5553, 14553), // response 2
  ];
}

Deno.test("attribution: the recorded production run is reported consistently", () => {
  const { chunks, events, sent } = record(productionRun());
  const r = attribute(chunks, events, sent);

  // The three interruption measurements are unchanged by the correction.
  assertEquals(r.metrics.secondActivityStartToInterruptedMs, 62);
  assertEquals(r.metrics.oldAudioAfterSecondActivityStartMs, 240);
  assertEquals(r.metrics.audioWithin300msAfterInterruptedMs, 0);

  // The corrected fields: turn 2 DID get a response.
  assertEquals(r.metrics.secondTurnGotResponse, true);
  assertEquals(r.metrics.secondTurnResponseMs, 33161);
  assertEquals(r.metrics.secondResponseFirstAudioAfterTurn2EndMs, 1013);
  assertEquals(r.metrics.audioBetweenInterruptedAndTurn2EndMs, 0);
  assertEquals(
    r.metrics.response2AttributionConfidence,
    "boundary (interrupted arrived before turn2 activityEnd)",
  );

  const phases = Object.fromEntries(r.phases.map((p) => [p.name, p.audioMs]));
  assertEquals(phases, {
    before_turn1_end: 0,
    response1: 1611,
    response1_after_barge: 240,
    between_interrupted_and_turn2_end: 0,
    response2: 33161,
  });
});

Deno.test("attribution: interrupted and the following turnComplete both close response 1", () => {
  const { chunks, events, sent } = record(productionRun());
  const r = attribute(chunks, events, sent);
  assertEquals(
    r.lifecycle.map((l) => [l.atMs, l.event, l.belongsTo]),
    [
      [3825, "interrupted", "response1 (closes the interrupted response)"],
      [3891, "turn_complete", "response1 (closes the interrupted response)"],
    ],
  );
});

Deno.test("attribution: a chunk sharing interrupted's millisecond counts by arrival order", () => {
  // Chunk then event at the same ms: old audio. Event then chunk: not old.
  const before = record([
    { kind: "event", atMs: 0, event: "turn1:activityEnd" },
    { kind: "event", atMs: 10, event: "turn2:activityStart" },
    { kind: "chunk", atMs: 50, bytes: 1920 },
    { kind: "event", atMs: 50, event: "interrupted" },
    { kind: "event", atMs: 90, event: "turn2:activityEnd" },
  ]);
  assertEquals(attribute(before.chunks, before.events, before.sent).metrics
    .oldAudioAfterSecondActivityStartMs, 40);

  const after = record([
    { kind: "event", atMs: 0, event: "turn1:activityEnd" },
    { kind: "event", atMs: 10, event: "turn2:activityStart" },
    { kind: "event", atMs: 50, event: "interrupted" },
    { kind: "chunk", atMs: 50, bytes: 1920 },
    { kind: "event", atMs: 90, event: "turn2:activityEnd" },
  ]);
  const m = attribute(after.chunks, after.events, after.sent).metrics;
  assertEquals(m.oldAudioAfterSecondActivityStartMs, 0);
  assertEquals(m.audioBetweenInterruptedAndTurn2EndMs, 40);
});

Deno.test("attribution: a turnComplete after response-2 audio belongs to response 2", () => {
  const run = productionRun();
  run.push({ kind: "event", atMs: 15000, event: "turn_complete" });
  const { chunks, events, sent } = record(run);
  const last = attribute(chunks, events, sent).lifecycle.at(-1)!;
  assertEquals([last.event, last.belongsTo], ["turn_complete", "response2"]);
});

Deno.test("attribution: without interrupted, response 2 is flagged weak", () => {
  const { chunks, events, sent } = record(
    productionRun().filter((i) => !(i.kind === "event" && i.event === "interrupted")),
  );
  const m = attribute(chunks, events, sent).metrics;
  assertEquals(m.secondActivityStartToInterruptedMs, null);
  assertEquals(m.response2AttributionConfidence, "weak (no interrupted before turn2 activityEnd)");
  // Old audio then runs to turn 2's activityEnd.
  assertEquals(m.oldAudioAfterSecondActivityStartMs, 240);
});
