// Response attribution for the production barge-in probe (PHASE6_PLAN §5.6.2).
//
// Pure: it takes the raw record the probe collected and returns derived,
// auditable fields. tests/probe_attribution_test.ts replays the recorded
// production run through it.
//
// RULES
//
// 1. Attribution comes from the LOCALLY SENT user-turn boundaries
//    (turn N activityStart / activityEnd), never from counting Gemini
//    lifecycle events. The first probe advanced a "generation" on every
//    `interrupted` AND every `turnComplete`; Gemini sends both to close the
//    interrupted response, so response 2 was labelled generation 3 and
//    reported as missing.
//
// 2. Membership is decided by ARRIVAL ORDER (`seq`), not by the millisecond
//    clock: on the production run the last old-response chunk and
//    `interrupted` arrived in the same millisecond. Timestamps are kept for
//    auditing and for the time-window metric.
//
// 3. The wire carries no response id, so phases are:
//      before_turn1_end                   [start,               turn1 activityEnd)
//      response1                          (turn1 activityEnd,   turn2 activityStart)
//      response1_after_barge              (turn2 activityStart, interrupted)      old audio
//      between_interrupted_and_turn2_end  (interrupted,         turn2 activityEnd) ambiguous
//      response2                          (turn2 activityEnd,   end)
//    If `interrupted` never arrives (or arrives after turn2 activityEnd),
//    response1_after_barge runs to turn2 activityEnd and the response2
//    attribution is flagged "weak".
//
// 4. Lifecycle: `interrupted` and the `turnComplete` that follows it before any
//    response-2 audio both CLOSE response 1. A `turnComplete` after response-2
//    audio began belongs to response 2.

export const MODEL_BYTES_PER_MS = 48; // 24 kHz s16le

export interface Stamp {
  seq: number;
  atMs: number;
}

export interface ChunkRecord extends Stamp {
  bytes: number;
}

export interface EventRecord extends Stamp {
  event: string;
}

export interface SentBoundaries {
  turn1ActivityStart: Stamp | null;
  turn1ActivityEnd: Stamp | null;
  turn2ActivityStart: Stamp | null;
  turn2ActivityEnd: Stamp | null;
}

export type PhaseName =
  | "before_turn1_end"
  | "response1"
  | "response1_after_barge"
  | "between_interrupted_and_turn2_end"
  | "response2";

export interface Phase {
  name: PhaseName;
  fromSeq: number | null;
  toSeq: number | null;
  fromMs: number | null;
  toMs: number | null;
  audioMs: number;
  chunks: number;
  firstAtMs: number | null;
  lastAtMs: number | null;
}

export function firstEventAfter(
  events: readonly EventRecord[],
  name: string,
  after: Stamp | null,
): EventRecord | null {
  if (after === null) return null;
  return events.find((e) => e.event === name && e.seq > after.seq) ?? null;
}

export function attribute(
  chunks: readonly ChunkRecord[],
  events: readonly EventRecord[],
  sent: SentBoundaries,
) {
  const t1End = sent.turn1ActivityEnd;
  const barge = sent.turn2ActivityStart;
  const t2End = sent.turn2ActivityEnd;
  const interrupted = firstEventAfter(events, "interrupted", barge);

  // Old audio runs to `interrupted`, unless turn 2 ended first.
  const oldEnds: Stamp | null = interrupted !== null && t2End !== null
    ? (interrupted.seq < t2End.seq ? interrupted : t2End)
    : t2End;

  const start: Stamp = { seq: -1, atMs: 0 };
  const bounds: Array<{ name: PhaseName; from: Stamp | null; to: Stamp | null }> = [
    { name: "before_turn1_end", from: start, to: t1End },
    { name: "response1", from: t1End, to: barge },
    { name: "response1_after_barge", from: barge, to: oldEnds },
    { name: "between_interrupted_and_turn2_end", from: oldEnds, to: t2End },
    { name: "response2", from: t2End, to: null },
  ];

  const phases: Phase[] = bounds.map((b) => {
    const inPhase = b.from === null ? [] : chunks.filter((c) =>
      c.seq > b.from!.seq && (b.to === null || c.seq < b.to.seq)
    );
    const bytes = inPhase.reduce((sum, c) => sum + c.bytes, 0);
    return {
      name: b.name,
      fromSeq: b.from?.seq ?? null,
      toSeq: b.to?.seq ?? null,
      fromMs: b.from?.atMs ?? null,
      toMs: b.to?.atMs ?? null,
      audioMs: Math.round(bytes / MODEL_BYTES_PER_MS),
      chunks: inPhase.length,
      firstAtMs: inPhase[0]?.atMs ?? null,
      lastAtMs: inPhase[inPhase.length - 1]?.atMs ?? null,
    };
  });
  const phase = (n: PhaseName) => phases.find((p) => p.name === n)!;
  const response2 = phase("response2");
  const r2First = t2End === null
    ? null
    : chunks.find((c) => c.seq > t2End.seq) ?? null;

  const lifecycle = events
    .filter((e) => e.event === "interrupted" || e.event === "turn_complete")
    .map((e) => {
      let belongsTo: string;
      if (barge === null || e.seq < barge.seq) {
        belongsTo = "response1 (before the barge-in)";
      } else if (r2First !== null && e.seq > r2First.seq) {
        belongsTo = "response2";
      } else {
        belongsTo = "response1 (closes the interrupted response)";
      }
      return { seq: e.seq, atMs: e.atMs, event: e.event, belongsTo };
    });

  // Time window, kept from the first version so runs stay comparable.
  const within300Bytes = interrupted === null ? 0 : chunks
    .filter((c) => c.seq > interrupted.seq && c.atMs - interrupted.atMs <= 300)
    .reduce((sum, c) => sum + c.bytes, 0);

  const boundaryConfidence = interrupted !== null && t2End !== null &&
    interrupted.seq < t2End.seq;

  return {
    boundaries: { ...sent, interrupted },
    phases,
    lifecycle,
    metrics: {
      secondActivityStartToInterruptedMs: interrupted !== null && barge !== null
        ? interrupted.atMs - barge.atMs
        : null,
      oldAudioAfterSecondActivityStartMs: phase("response1_after_barge").audioMs,
      audioWithin300msAfterInterruptedMs: Math.round(within300Bytes / MODEL_BYTES_PER_MS),
      audioBetweenInterruptedAndTurn2EndMs: phase("between_interrupted_and_turn2_end").audioMs,
      secondTurnGotResponse: response2.chunks > 0,
      secondTurnResponseMs: response2.audioMs,
      secondResponseFirstAudioAfterTurn2EndMs: r2First !== null && t2End !== null
        ? r2First.atMs - t2End.atMs
        : null,
      response2AttributionConfidence: boundaryConfidence
        ? "boundary (interrupted arrived before turn2 activityEnd)"
        : "weak (no interrupted before turn2 activityEnd)",
    },
  };
}
