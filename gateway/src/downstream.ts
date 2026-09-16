// Ordered, credit-gated downstream scheduling (PHASE6_PLAN §3.3).
//
// Two lanes:
//   ordered   per-turn items in FIFO order: speech_start, model audio,
//             interrupted, turn_complete, turn-scoped errors. An audio item
//             at the head waits for credit, and everything behind it waits
//             too — so turn_complete(N) can never overtake N's audio.
//   priority  connection-scoped control (pong, session_end, connection
//             errors): always deliverable, credit or not.
//
// Nothing here blocks: next() returns what may be sent now, or null.

import { CreditLedger } from "./credit.ts";

type OrderedItem =
  | { kind: "control"; turn: number; text: string }
  | { kind: "audio"; turn: number; frame: Uint8Array; pcmBytes: number };

export type Sendable =
  | { kind: "text"; text: string }
  | { kind: "binary"; data: Uint8Array; turn: number; pcmBytes: number };

export class DownstreamScheduler {
  #ledger: CreditLedger;
  #priority: string[] = [];
  #ordered: OrderedItem[] = [];
  #queuedAudio = new Map<number, number>();

  constructor(ledger: CreditLedger) {
    this.#ledger = ledger;
  }

  get length(): number {
    return this.#priority.length + this.#ordered.length;
  }

  queuedAudioBytes(turn: number): number {
    return this.#queuedAudio.get(turn) ?? 0;
  }

  pushPriority(text: string): void {
    this.#priority.push(text);
  }

  pushControl(turn: number, text: string): void {
    this.#ordered.push({ kind: "control", turn, text });
  }

  pushAudio(turn: number, frame: Uint8Array, pcmBytes: number): void {
    this.#ordered.push({ kind: "audio", turn, frame, pcmBytes });
    this.#queuedAudio.set(turn, this.queuedAudioBytes(turn) + pcmBytes);
  }

  // Drops every unsent item of `turn`. Unsent audio never consumed credit, so
  // purging it needs no credit adjustment.
  purgeTurn(turn: number): { items: number; audioBytes: number } {
    let items = 0;
    let audioBytes = 0;
    this.#ordered = this.#ordered.filter((item) => {
      if (item.turn !== turn) return true;
      items += 1;
      if (item.kind === "audio") audioBytes += item.pcmBytes;
      return false;
    });
    this.#queuedAudio.delete(turn);
    return { items, audioBytes };
  }

  // LAN FAKE MODE TEST CONTROL ONLY (FAKE_VIOLATE_CREDIT): the audio frame at
  // the head of the ordered lane, sent WITHOUT credit. Null unless the head is
  // audio and nothing is waiting in the priority lane.
  forceHeadAudio(): Sendable | null {
    const head = this.#ordered[0];
    if (this.#priority.length > 0 || head === undefined || head.kind !== "audio") return null;
    this.#ledger.forceSpend(head.pcmBytes);
    this.#ordered.shift();
    const left = this.queuedAudioBytes(head.turn) - head.pcmBytes;
    if (left > 0) this.#queuedAudio.set(head.turn, left);
    else this.#queuedAudio.delete(head.turn);
    return { kind: "binary", data: head.frame, turn: head.turn, pcmBytes: head.pcmBytes };
  }

  next(): Sendable | null {
    const priority = this.#priority.shift();
    if (priority !== undefined) return { kind: "text", text: priority };

    const head = this.#ordered[0];
    if (head === undefined) return null;
    if (head.kind === "control") {
      this.#ordered.shift();
      return { kind: "text", text: head.text };
    }
    // Audio at the head: only within credit. If it does not fit, the whole
    // ordered lane waits — that is what keeps the order.
    if (!this.#ledger.trySpend(head.pcmBytes)) return null;
    this.#ordered.shift();
    const left = this.queuedAudioBytes(head.turn) - head.pcmBytes;
    if (left > 0) this.#queuedAudio.set(head.turn, left);
    else this.#queuedAudio.delete(head.turn);
    return {
      kind: "binary",
      data: head.frame,
      turn: head.turn,
      pcmBytes: head.pcmBytes,
    };
  }
}
