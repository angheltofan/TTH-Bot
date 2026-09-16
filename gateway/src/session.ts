// One device connection: the tth.v1 protocol on one side, a Gemini Live
// session on the other (PHASE6_PLAN §3, §5).
//
// Ordering guarantees implemented here:
//   upstream   device frames are forwarded to Gemini in arrival order:
//              activityStart · audio... · activityEnd. While Gemini is
//              (re)connecting they wait in one FIFO, so reconnecting cannot
//              reorder them.
//   downstream per turn, in the ordered lane: speech_start · audio... ·
//              turn_complete, with audio released only against device credit
//              (DownstreamScheduler). pong / session_end / connection errors
//              use the priority lane and are deliverable at zero credit.
//   cancel     cancel(N) purges N's unsent downstream items and raises a
//              FENCE: every model-audio chunk is dropped (and counted) until
//              Gemini acknowledges the interruption (interrupted or
//              turnComplete), or the fence times out. Without the fence,
//              audio Gemini had already sent for N could be attributed to N+1.
//
// All dependencies are injected (sockets, token minting, Gemini connector,
// timers, logger), so the whole session runs against fakes in the tests.

import { ResolvedActivity } from "./activities.ts";
import { CreditLedger } from "./credit.ts";
import { DownstreamScheduler } from "./downstream.ts";
import {
  ACTIVITY_END,
  ACTIVITY_START,
  audioMessage,
  buildSetupMessage,
  GeminiConnector,
  GeminiEvent,
  GeminiLink,
} from "./gemini.ts";
import { LIMITS } from "./limits.ts";
import { Logger } from "./log.ts";
import {
  encodeAudioFrame,
  encodeError,
  encodeInterrupted,
  encodePong,
  encodeReady,
  encodeSessionEnd,
  encodeSpeechStart,
  encodeTurnComplete,
  KIND_MODEL_AUDIO,
  KIND_USER_AUDIO,
  MAX_DOWN_PCM_BYTES,
  parseAudioFrame,
  parseDeviceMessage,
} from "./protocol.ts";
import { TurnRegistry } from "./turns.ts";

export interface DeviceSocket {
  sendText(text: string): void;
  sendBinary(data: Uint8Array): void;
  bufferedAmount(): number;
  close(code: number, reason: string): void;
}

export interface Timers {
  now(): number;
  set(fn: () => void, ms: number): number;
  clear(id: number): void;
}

export interface SessionDeps {
  deviceId: string;
  sessionId: string;
  resolved: ResolvedActivity;
  systemInstruction: string;
  device: DeviceSocket;
  mintToken: () => Promise<string>;
  connectGemini: GeminiConnector;
  timers: Timers;
  log: Logger;
  // Per-device turn rate limit; true = allowed.
  allowTurn?: (nowMs: number) => boolean;
  // LAN FAKE MODE ONLY (loadConfig refuses FAKE_* settings otherwise).
  testControls?: SessionTestControls;
}

// Deterministic credit controls for the device's physical gates (Step 6.3).
export interface SessionTestControls {
  // Once the device's credit is exhausted, credit it returns is held this long
  // before it is applied: the zero-credit state lasts long enough to observe.
  creditHoldMs?: number;
  // Shared across sessions: how many model audio frames may still be sent
  // BEYOND the device's credit (one per zero-credit stall), to prove the
  // device refuses them and recovers.
  overCreditFrames?: { remaining: number };
}

export interface SessionStats {
  staleModelFrames: number;
  staleUserFrames: number;
  unknownMessages: number;
  lateCancels: number;
  geminiOpens: number;
  // turnCompletes consumed because they closed an interrupted response.
  closedInterruptedResponses: number;
}

type Phase = "awaiting_hello" | "connecting" | "ready" | "closed";

const HELLO_TIMEOUT_MS = 10_000;
const PUMP_RETRY_MS = 20;
// base64 of a full 46 s turn plus JSON overhead.
const MAX_PENDING_UPSTREAM_CHARS = 2_200_000;

export class DeviceSession {
  #d: SessionDeps;
  #phase: Phase = "awaiting_hello";
  #ledger: CreditLedger | null = null;
  #down: DownstreamScheduler | null = null;
  #turns = new TurnRegistry();

  #gemini: GeminiLink | null = null;
  #geminiReady = false;
  #geminiConnecting = false;
  #geminiOpenedAt = 0;
  #geminiRetire = false; // go_away / max age: close at the next boundary
  #pending: string[] = [];
  #pendingChars = 0;

  #up: { turn: number; frames: number; bytes: number } | null = null;
  #response: { turn: number; frames: number; bytes: number; started: boolean } | null =
    null;
  #fence: number | null = null;
  #fenceSince = 0;
  // Set when `interrupted` lowered the fence. Gemini follows it with a
  // turnComplete that closes the INTERRUPTED response (observed on the
  // production path 66 ms later, before any new audio). Until new-response
  // audio arrives, that turnComplete must not be taken as the end of the
  // next turn — it may reach us after the next turn's turn_end.
  #closingInterrupted = false;
  // FAKE_CREDIT_HOLD_MS: credit returned while held.
  #holdingCredit = false;
  #heldCredit = 0;

  #timers = new Map<string, number>();
  #stats: SessionStats = {
    staleModelFrames: 0,
    staleUserFrames: 0,
    unknownMessages: 0,
    lateCancels: 0,
    geminiOpens: 0,
    closedInterruptedResponses: 0,
  };

  constructor(deps: SessionDeps) {
    this.#d = deps;
    this.#setTimer("hello", HELLO_TIMEOUT_MS, () => {
      if (this.#phase === "awaiting_hello") this.#protocolError("hello_timeout");
    });
    this.#setTimer("max_age", LIMITS.deviceMaxAgeMs, () => {
      // The device should have reconnected at ~55 min (Cloud Run's request
      // timeout is 60 min). Nudge it; close for good 2 min later.
      this.#down?.pushPriority(encodeSessionEnd("max_age"));
      this.#pump();
      this.#setTimer("max_age_close", 2 * 60_000, () => this.close(1000, "max_age"));
    });
  }

  get stats(): SessionStats {
    return { ...this.#stats };
  }

  get phase(): Phase {
    return this.#phase;
  }

  // Not a property check TypeScript can narrow: the phase changes across awaits.
  #isClosed(): boolean {
    return this.#phase === "closed";
  }

  // --- device input -------------------------------------------------------------

  onText(text: string): void {
    if (this.#phase === "closed") return;
    const parsed = parseDeviceMessage(text);
    if (!parsed.ok) return this.#protocolError(parsed.error);
    const msg = parsed.msg;

    if (msg.t === "hello") {
      if (this.#phase !== "awaiting_hello") return this.#protocolError("duplicate_hello");
      this.#clearTimer("hello");
      this.#ledger = new CreditLedger(msg.credit);
      this.#down = new DownstreamScheduler(this.#ledger);
      this.#phase = "connecting";
      this.#d.log.info("session_hello", {
        device: this.#d.deviceId,
        session: this.#d.sessionId,
        fw: msg.fw,
        credit: msg.credit,
        activity: this.#d.resolved.activity.id,
      });
      if (this.#d.resolved.convertedFromFreeConversation) {
        this.#d.log.info("mode_converted", {
          device: this.#d.deviceId,
          activity: this.#d.resolved.activity.id,
          mode: "free_conversation_to_push_to_talk",
          converted: true,
        });
      }
      this.#openGemini();
      return;
    }
    if (this.#phase !== "ready") return this.#protocolError("not_ready");

    switch (msg.t) {
      case "turn_start":
        return this.#turnStart(msg.turn);
      case "turn_end":
        return this.#turnEnd(msg.turn, msg.frames, msg.bytes);
      case "cancel":
        return this.#cancel(msg.turn);
      case "credit": {
        const holdMs = this.#d.testControls?.creditHoldMs ?? 0;
        if (holdMs > 0 && (this.#holdingCredit || this.#ledger!.available <= 0)) {
          if (!this.#holdingCredit) {
            this.#holdingCredit = true;
            this.#d.log.info("test_credit_hold", { device: this.#d.deviceId, ms: holdMs });
            this.#setTimer("credit_hold", holdMs, () => {
              const bytes = this.#heldCredit;
              this.#holdingCredit = false;
              this.#heldCredit = 0;
              this.#applyCredit(bytes);
            });
          }
          this.#heldCredit += msg.bytes;
          return;
        }
        return this.#applyCredit(msg.bytes);
      }
      case "ping":
        this.#down!.pushPriority(encodePong(msg.ts));
        return this.#pump();
      case "unknown":
        this.#stats.unknownMessages += 1;
        return;
    }
  }

  onBinary(data: Uint8Array): void {
    if (this.#phase === "closed") return;
    if (this.#phase !== "ready") return this.#protocolError("not_ready");
    const parsed = parseAudioFrame(data, KIND_USER_AUDIO);
    if (!parsed.ok) return this.#protocolError(`frame_${parsed.error}`);
    const { turn, pcm } = parsed.frame;
    const up = this.#up;
    if (up === null || up.turn !== turn) {
      // A frame for a turn that is not open: cancelled or unknown. Dropped
      // and counted, never forwarded.
      this.#stats.staleUserFrames += 1;
      return;
    }
    if (up.bytes + pcm.length > LIMITS.maxUserAudioBytesPerTurn) {
      this.#d.log.warn("turn_too_long", { device: this.#d.deviceId, turn, bytes: up.bytes });
      this.#down!.pushControl(turn, encodeError("turn_too_long", true, turn));
      this.#cancel(turn);
      this.#pump();
      return;
    }
    up.frames += 1;
    up.bytes += pcm.length;
    this.#sendGemini(audioMessage(pcm));
  }

  onClose(): void {
    this.close(1000, "device_closed");
  }

  close(code: number, reason: string): void {
    if (this.#phase === "closed") return;
    this.#phase = "closed";
    for (const id of this.#timers.values()) this.#d.timers.clear(id);
    this.#timers.clear();
    this.#closeGemini(reason);
    this.#d.log.info("session_closed", {
      device: this.#d.deviceId,
      session: this.#d.sessionId,
      reason,
      stale: this.#stats.staleModelFrames,
    });
    try {
      this.#d.device.close(code, reason);
    } catch {
      // already closed
    }
  }

  // Replaced by a newer connection from the same device.
  replace(): void {
    this.#down?.pushPriority(encodeSessionEnd("replaced"));
    this.#pump();
    this.close(1000, "replaced");
  }

  // --- turns ------------------------------------------------------------------------

  #turnStart(turn: number): void {
    if (this.#d.allowTurn && !this.#d.allowTurn(this.#d.timers.now())) {
      this.#down!.pushControl(turn, encodeError("rate_limited", true, turn));
      return this.#pump();
    }
    const check = this.#turns.validateStart(turn);
    if (check !== "ok") {
      this.#d.log.warn("bad_turn", { device: this.#d.deviceId, turn, reason: check });
      this.#down!.pushControl(turn, encodeError("bad_turn", false, turn));
      return this.#pump();
    }
    // A new turn while the previous response is still streaming, without a
    // cancel first: treat it as an implicit cancel of that response.
    if (this.#response !== null) this.#cancel(this.#response.turn);

    this.#turns.start(turn);
    this.#up = { turn, frames: 0, bytes: 0 };
    this.#clearTimer("idle");
    if (this.#geminiRetire && this.#geminiReady) this.#closeGemini("retired");
    this.#ensureGemini();
    this.#sendGemini(ACTIVITY_START);
    this.#d.log.info("turn_start", { device: this.#d.deviceId, turn });
  }

  #turnEnd(turn: number, frames: number, bytes: number): void {
    const up = this.#up;
    if (up === null || up.turn !== turn) {
      this.#stats.staleUserFrames += 1;
      return;
    }
    this.#sendGemini(ACTIVITY_END);
    this.#turns.closeUpstream(turn);
    this.#up = null;
    if (up.frames !== frames || up.bytes !== bytes) {
      // Something was lost between the device and here: never answer a
      // partial question as if it were complete.
      this.#d.log.warn("turn_incomplete", {
        device: this.#d.deviceId,
        turn,
        frames: up.frames,
        bytes: up.bytes,
      });
      this.#down!.pushControl(turn, encodeError("turn_incomplete", true, turn));
      this.#raiseFence(turn);
      return this.#pump();
    }
    this.#response = { turn, frames: 0, bytes: 0, started: false };
    this.#d.log.info("turn_end", { device: this.#d.deviceId, turn, frames, bytes });
  }

  #cancel(turn: number): void {
    if (this.#up !== null && this.#up.turn === turn) {
      // Mid-upload: close Gemini's activity; whatever it answers is fenced.
      this.#sendGemini(ACTIVITY_END);
      this.#turns.closeUpstream(turn);
      this.#up = null;
      this.#raiseFence(turn);
    } else if (this.#response !== null && this.#response.turn === turn) {
      const purged = this.#down!.purgeTurn(turn);
      this.#response = null;
      this.#raiseFence(turn);
      this.#d.log.info("turn_cancelled", {
        device: this.#d.deviceId,
        turn,
        purged: purged.items,
        bytes: purged.audioBytes,
      });
    } else {
      // Gemini already finished the response, but its audio may still be
      // queued behind the device's credit. Barge-in must not keep streaming
      // it: purge what is unsent. No fence: nothing more will come from Gemini.
      const purged = this.#down!.purgeTurn(turn);
      if (purged.items > 0) {
        this.#d.log.info("turn_cancelled", {
          device: this.#d.deviceId,
          turn,
          purged: purged.items,
          bytes: purged.audioBytes,
        });
      } else {
        this.#stats.lateCancels += 1;
      }
    }
    this.#pump();
  }

  #applyCredit(bytes: number): void {
    if (this.#ledger!.applyReturn(bytes) === "over_return") {
      return this.#protocolError("credit_over_return");
    }
    this.#pump();
  }

  // FAKE_VIOLATE_CREDIT: one audio frame beyond the device's credit, while the
  // budget lasts. False when nothing was sent.
  #sendOverCredit(down: DownstreamScheduler): boolean {
    const budget = this.#d.testControls?.overCreditFrames;
    if (budget === undefined || budget.remaining <= 0) return false;
    const forced = down.forceHeadAudio();
    if (forced === null || forced.kind !== "binary") return false;
    budget.remaining -= 1;
    this.#d.log.warn("test_over_credit_frame", {
      device: this.#d.deviceId,
      turn: forced.turn,
      bytes: forced.pcmBytes,
    });
    this.#d.device.sendBinary(forced.data);
    return true;
  }

  #raiseFence(turn: number): void {
    this.#fence = turn;
    this.#closingInterrupted = false;
    this.#fenceSince = this.#d.timers.now();
    this.#setTimer("fence", LIMITS.interruptFenceTimeoutMs, () => {
      if (this.#fence === null) return;
      this.#d.log.warn("interrupt_fence_timeout", { device: this.#d.deviceId, turn });
      this.#fence = null;
    });
  }

  #lowerFence(): void {
    if (this.#fence === null) return;
    this.#d.log.info("interrupt_acknowledged", {
      device: this.#d.deviceId,
      turn: this.#fence,
      ms: this.#d.timers.now() - this.#fenceSince,
    });
    this.#fence = null;
    this.#clearTimer("fence");
  }

  // --- Gemini ------------------------------------------------------------------------

  #ensureGemini(): void {
    if (this.#gemini !== null || this.#geminiConnecting) return;
    this.#openGemini();
  }

  #openGemini(): void {
    this.#geminiConnecting = true;
    this.#geminiReady = false;
    this.#geminiRetire = false;
    this.#stats.geminiOpens += 1;
    this.#setTimer("setup", LIMITS.geminiSetupTimeoutMs, () => this.#geminiFailed("gemini_setup_timeout"));
    const opening = (async () => {
      const token = await this.#d.mintToken();
      if (this.#isClosed()) return;
      const link = await this.#d.connectGemini(
        token,
        buildSetupMessage(this.#d.systemInstruction),
        {
          onEvent: (event) => {
            if (this.#gemini === link) this.#onGemini(event);
          },
          onClose: (code) => {
            if (this.#gemini === link) this.#onGeminiClose(code);
          },
        },
      );
      if (this.#isClosed()) {
        link.close();
        return;
      }
      this.#gemini = link;
      this.#geminiOpenedAt = this.#d.timers.now();
    })();
    opening.catch(() => this.#geminiFailed("gemini_unavailable"));
  }

  #geminiFailed(code: string): void {
    if (this.#phase === "closed") return;
    this.#clearTimer("setup");
    this.#geminiConnecting = false;
    this.#d.log.warn("gemini_failed", { device: this.#d.deviceId, code });
    this.#closeGemini(code);
    if (this.#phase === "connecting") {
      // Never became ready: tell the device and let it back off.
      this.#d.device.sendText(encodeError(code, true));
      this.close(1011, code);
      return;
    }
    this.#failActiveTurn(code);
  }

  #failActiveTurn(code: string): void {
    const turn = this.#up?.turn ?? this.#response?.turn;
    this.#pending = [];
    this.#pendingChars = 0;
    if (turn !== undefined) {
      if (this.#up) this.#turns.closeUpstream(this.#up.turn);
      this.#down!.purgeTurn(turn);
      this.#down!.pushControl(turn, encodeError(code, true, turn));
    }
    this.#up = null;
    this.#response = null;
    this.#fence = null;
    this.#closingInterrupted = false;
    this.#pump();
  }

  #sendGemini(text: string): void {
    if (this.#gemini !== null && this.#geminiReady) {
      this.#gemini.send(text);
      return;
    }
    // Waiting for (re)connection: one FIFO keeps activityStart, audio and
    // activityEnd in order.
    this.#pendingChars += text.length;
    if (this.#pendingChars > MAX_PENDING_UPSTREAM_CHARS) {
      return this.#geminiFailed("gemini_backlog");
    }
    this.#pending.push(text);
  }

  #onGemini(event: GeminiEvent): void {
    switch (event.type) {
      case "setup_complete": {
        this.#clearTimer("setup");
        this.#geminiConnecting = false;
        this.#geminiReady = true;
        this.#d.log.info("gemini_ready", { device: this.#d.deviceId });
        const pending = this.#pending;
        this.#pending = [];
        this.#pendingChars = 0;
        for (const text of pending) this.#gemini!.send(text);
        if (this.#phase === "connecting") {
          this.#phase = "ready";
          this.#down!.pushPriority(
            encodeReady(this.#d.sessionId, this.#d.resolved.activity.id),
          );
          this.#pump();
          this.#armIdle();
        }
        return;
      }
      case "audio":
        return this.#onModelAudio(event.pcm);
      case "interrupted":
        if (this.#fence !== null) {
          this.#lowerFence();
          this.#closingInterrupted = true;
          return;
        }
        if (this.#response !== null) {
          this.#down!.pushControl(this.#response.turn, encodeInterrupted(this.#response.turn));
          this.#pump();
        }
        return;
      case "turn_complete":
        if (this.#fence !== null) return this.#lowerFence();
        if (this.#closingInterrupted) {
          // Closes the interrupted response — never the turn after it.
          this.#closingInterrupted = false;
          this.#stats.closedInterruptedResponses += 1;
          this.#d.log.info("interrupted_response_closed", { device: this.#d.deviceId });
          return;
        }
        if (this.#response !== null) {
          const r = this.#response;
          this.#response = null;
          this.#down!.pushControl(r.turn, encodeTurnComplete(r.turn, r.frames, r.bytes));
          this.#d.log.info("response_complete", {
            device: this.#d.deviceId,
            turn: r.turn,
            frames: r.frames,
            bytes: r.bytes,
          });
          this.#pump();
          this.#afterResponse();
        }
        return;
      case "go_away":
        this.#geminiRetire = true;
        if (this.#up === null && this.#response === null) this.#closeGemini("go_away");
        return;
      case "error":
        this.#d.log.warn("gemini_error", { device: this.#d.deviceId, code: event.message });
        this.#closeGemini("gemini_error");
        this.#failActiveTurn("gemini_error");
        return;
    }
  }

  #onModelAudio(pcm: Uint8Array): void {
    if (this.#fence !== null || this.#response === null) {
      this.#stats.staleModelFrames += 1;
      return;
    }
    const r = this.#response;
    // Audio accepted for the current response: any turnComplete from here on
    // belongs to it.
    this.#closingInterrupted = false;
    if (r.bytes + pcm.length > LIMITS.maxModelAudioBytesPerTurn) {
      this.#d.log.warn("response_too_long", { device: this.#d.deviceId, turn: r.turn });
      this.#down!.pushControl(r.turn, encodeError("response_too_long", false, r.turn));
      this.#down!.pushControl(r.turn, encodeTurnComplete(r.turn, r.frames, r.bytes));
      this.#response = null;
      this.#raiseFence(r.turn);
      return this.#pump();
    }
    if (!r.started) {
      r.started = true;
      this.#down!.pushControl(r.turn, encodeSpeechStart(r.turn));
    }
    const even = pcm.length - (pcm.length % 2);
    for (let offset = 0; offset < even; offset += MAX_DOWN_PCM_BYTES) {
      const chunk = pcm.subarray(offset, Math.min(even, offset + MAX_DOWN_PCM_BYTES));
      this.#down!.pushAudio(r.turn, encodeAudioFrame(KIND_MODEL_AUDIO, r.turn, chunk), chunk.length);
      r.frames += 1;
      r.bytes += chunk.length;
    }
    this.#pump();
  }

  #onGeminiClose(code: number): void {
    const wasReady = this.#geminiReady;
    this.#gemini = null;
    this.#geminiReady = false;
    this.#d.log.info("gemini_closed", { device: this.#d.deviceId, status: code });
    if (!wasReady) return this.#geminiFailed("gemini_unavailable");
    if (this.#up !== null || this.#response !== null) this.#failActiveTurn("gemini_closed");
  }

  #afterResponse(): void {
    const age = this.#d.timers.now() - this.#geminiOpenedAt;
    if (this.#geminiRetire || age >= LIMITS.geminiMaxAgeMs) {
      // Between turns only: the next turn_start reopens a fresh session.
      this.#closeGemini(this.#geminiRetire ? "go_away" : "max_age");
    }
    this.#armIdle();
  }

  #armIdle(): void {
    this.#setTimer("idle", LIMITS.geminiIdleCloseMs, () => {
      if (this.#up === null && this.#response === null && this.#fence === null) {
        this.#closeGemini("idle");
      }
    });
  }

  #closeGemini(reason: string): void {
    const link = this.#gemini;
    this.#gemini = null;
    this.#geminiReady = false;
    this.#geminiConnecting = false;
    if (link !== null) {
      this.#d.log.info("gemini_close", { device: this.#d.deviceId, reason });
      try {
        link.close();
      } catch {
        // ignore
      }
    }
  }

  // --- output ----------------------------------------------------------------------

  #pump(): void {
    const down = this.#down;
    if (down === null || this.#phase === "closed") return;
    while (this.#d.device.bufferedAmount() < LIMITS.sendHighWaterBytes) {
      const item = down.next();
      if (item === null) {
        if (this.#sendOverCredit(down)) continue;
        return;
      }
      if (item.kind === "text") this.#d.device.sendText(item.text);
      else this.#d.device.sendBinary(item.data);
    }
    // The socket is backed up: try again shortly rather than queue in it.
    if (down.length > 0 && !this.#timers.has("pump")) {
      this.#setTimer("pump", PUMP_RETRY_MS, () => this.#pump());
    }
  }

  #protocolError(code: string): void {
    this.#d.log.warn("protocol_error", { device: this.#d.deviceId, code });
    try {
      this.#d.device.sendText(encodeError("protocol", false));
    } catch {
      // ignore
    }
    this.close(1002, "protocol");
  }

  #setTimer(name: string, ms: number, fn: () => void): void {
    this.#clearTimer(name);
    const id = this.#d.timers.set(() => {
      this.#timers.delete(name);
      fn();
    }, ms);
    this.#timers.set(name, id);
  }

  #clearTimer(name: string): void {
    const id = this.#timers.get(name);
    if (id !== undefined) {
      this.#d.timers.clear(id);
      this.#timers.delete(name);
    }
  }
}
