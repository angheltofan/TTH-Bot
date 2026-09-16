// Fakes for DeviceSession tests: a device socket, a Gemini link, and a clock
// that only moves when a test says so. Shared by session_test.ts and
// fence_replay_test.ts.

import { assert, assertEquals } from "./assert.ts";
import { Activity } from "../src/activities.ts";
import {
  ACTIVITY_END,
  ACTIVITY_START,
  GeminiConnector,
  GeminiEvent,
  GeminiHandlers,
  GeminiLink,
} from "../src/gemini.ts";
import { createLogger } from "../src/log.ts";
import {
  encodeAudioFrame,
  KIND_MODEL_AUDIO,
  KIND_USER_AUDIO,
  parseAudioFrame,
} from "../src/protocol.ts";
import { DeviceSession, DeviceSocket, Timers } from "../src/session.ts";

export class FakeTimers implements Timers {
  nowMs = 0;
  #next = 1;
  #tasks = new Map<number, { at: number; fn: () => void }>();
  now() {
    return this.nowMs;
  }
  set(fn: () => void, ms: number) {
    const id = this.#next++;
    this.#tasks.set(id, { at: this.nowMs + ms, fn });
    return id;
  }
  clear(id: number) {
    this.#tasks.delete(id);
  }
  advance(ms: number) {
    const target = this.nowMs + ms;
    for (;;) {
      let due: [number, { at: number; fn: () => void }] | undefined;
      for (const entry of this.#tasks) {
        if (entry[1].at <= target && (due === undefined || entry[1].at < due[1].at)) due = entry;
      }
      if (due === undefined) break;
      this.#tasks.delete(due[0]);
      this.nowMs = due[1].at;
      due[1].fn();
    }
    this.nowMs = target;
  }
}

export type Sent =
  // deno-lint-ignore no-explicit-any
  | { kind: "text"; msg: any }
  | { kind: "binary"; turn: number; pcmBytes: number };

export class FakeDevice implements DeviceSocket {
  sent: Sent[] = [];
  buffered = 0;
  closed: { code: number; reason: string } | null = null;
  sendText(text: string) {
    this.sent.push({ kind: "text", msg: JSON.parse(text) });
  }
  sendBinary(data: Uint8Array) {
    const parsed = parseAudioFrame(data, KIND_MODEL_AUDIO);
    assert(parsed.ok, "the gateway sent an invalid model frame");
    this.sent.push({ kind: "binary", turn: parsed.frame.turn, pcmBytes: parsed.frame.pcm.length });
  }
  bufferedAmount() {
    return this.buffered;
  }
  close(code: number, reason: string) {
    this.closed = { code, reason };
  }
  types(): string[] {
    return this.sent.map((s) => (s.kind === "text" ? s.msg.t : `audio:${s.turn}:${s.pcmBytes}`));
  }
  take(): string[] {
    const out = this.types();
    this.sent = [];
    return out;
  }
}

export class FakeLink implements GeminiLink {
  sent: string[] = [];
  closed = false;
  constructor(readonly token: string, readonly setup: string, readonly handlers: GeminiHandlers) {}
  send(text: string) {
    this.sent.push(text);
  }
  close() {
    this.closed = true;
  }
  emit(event: GeminiEvent) {
    this.handlers.onEvent(event);
  }
}

export class FakeGemini {
  links: FakeLink[] = [];
  connector: GeminiConnector = (token, setup, handlers) => {
    const link = new FakeLink(token, setup, handlers);
    this.links.push(link);
    return Promise.resolve(link);
  };
  get last(): FakeLink {
    return this.links[this.links.length - 1];
  }
}

export const flush = () => new Promise((resolve) => setTimeout(resolve, 0));

export const ACTIVITY: Activity = {
  id: "a-1",
  title: "Baschet",
  type: "lesson",
  prompt: "p",
  participants: [],
  interactionMode: "push_to_talk",
  enabled: true,
  sortOrder: 0,
};

export function harness(
  opts: { converted?: boolean; allowTurn?: () => boolean; system?: string } = {},
) {
  const timers = new FakeTimers();
  const device = new FakeDevice();
  const gemini = new FakeGemini();
  const logs: string[] = [];
  let mints = 0;
  const session = new DeviceSession({
    deviceId: "core2-01",
    sessionId: "s-1",
    resolved: { activity: ACTIVITY, convertedFromFreeConversation: opts.converted ?? false },
    systemInstruction: opts.system ?? "SYS",
    device,
    mintToken: () => Promise.resolve(`tok-${++mints}`),
    connectGemini: gemini.connector,
    timers,
    log: createLogger((line) => logs.push(line), () => "T"),
    allowTurn: opts.allowTurn,
  });
  return { timers, device, gemini, logs, session };
}

export type H = ReturnType<typeof harness>;

export async function ready(h: H, credit = 192000) {
  h.session.onText(
    `{"t":"hello","proto":1,"fw":"core2-6.0","in":"s16le/16000/1","out":"s16le/24000/1","maxDown":1920,"credit":${credit}}`,
  );
  await flush();
  h.gemini.last.emit({ type: "setup_complete" });
  assertEquals(h.device.take(), ["ready"]);
}

export function up(turn: number, bytes = 640, fill = 1): Uint8Array {
  return encodeAudioFrame(KIND_USER_AUDIO, turn, new Uint8Array(bytes).fill(fill));
}

// A complete user turn of `frames` 640 B frames.
export function userTurn(h: H, turn: number, frames = 2) {
  h.session.onText(`{"t":"turn_start","turn":${turn}}`);
  for (let i = 0; i < frames; i++) h.session.onBinary(up(turn));
  h.session.onText(`{"t":"turn_end","turn":${turn},"frames":${frames},"bytes":${frames * 640}}`);
}

export function model(bytes: number): GeminiEvent {
  return { type: "audio", pcm: new Uint8Array(bytes) };
}

export function geminiKinds(link: FakeLink): string[] {
  return link.sent.map((s) =>
    s === ACTIVITY_START
      ? "start"
      : s === ACTIVITY_END
      ? "end"
      : JSON.parse(s).realtimeInput?.audio
      ? "audio"
      : "?"
  );
}
