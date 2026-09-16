// tth.v1 wire protocol — gateway side.
//
// Mirrors firmware/core2/lib/tth_core/include/tth/GatewayProtocol.h. The
// message strings in tests/protocol_test.ts are the same byte-exact vectors
// as firmware/core2/test/test_gateway_protocol, so the two implementations
// cannot drift apart silently.
//
// Binary frames: audio only, 8-byte header
//   0 kind (0x01 user audio up, 0x02 model audio down) · 1 flags = 0 ·
//   2-3 reserved = 0 · 4-7 turn_id uint32 LE (0 reserved) · 8.. PCM s16le
// Control frames: flat JSON text, at most 512 bytes.

export const PROTOCOL_VERSION = 1;
export const SUBPROTOCOL = "tth.v1";

export const AUDIO_HEADER_BYTES = 8;
export const KIND_USER_AUDIO = 0x01;
export const KIND_MODEL_AUDIO = 0x02;
export const MAX_UP_PCM_BYTES = 640;
export const MAX_DOWN_PCM_BYTES = 1920;
export const MAX_CONTROL_BYTES = 512;

export const FORMAT_UP = "s16le/16000/1";
export const FORMAT_DOWN = "s16le/24000/1";

const U32_MAX = 0xffffffff;
const encoder = new TextEncoder();

export function isU32(value: unknown): value is number {
  return typeof value === "number" && Number.isInteger(value) && value >= 0 &&
    value <= U32_MAX;
}

export function isValidTurn(value: unknown): value is number {
  return isU32(value) && value !== 0;
}

export function utf8Length(text: string): number {
  return encoder.encode(text).length;
}

// --- binary -----------------------------------------------------------------

export type FrameError =
  | "too_short"
  | "bad_kind"
  | "bad_flags"
  | "bad_reserved"
  | "zero_turn"
  | "empty_payload"
  | "odd_payload"
  | "oversize";

export interface AudioFrame {
  kind: number;
  turn: number;
  pcm: Uint8Array;
}

export function encodeAudioFrame(
  kind: number,
  turn: number,
  pcm: Uint8Array,
): Uint8Array {
  if (kind !== KIND_USER_AUDIO && kind !== KIND_MODEL_AUDIO) {
    throw new RangeError("unknown frame kind");
  }
  if (!isValidTurn(turn)) throw new RangeError("invalid turn id");
  const limit = kind === KIND_USER_AUDIO ? MAX_UP_PCM_BYTES : MAX_DOWN_PCM_BYTES;
  if (pcm.length === 0 || pcm.length % 2 !== 0 || pcm.length > limit) {
    throw new RangeError("invalid PCM length");
  }
  const out = new Uint8Array(AUDIO_HEADER_BYTES + pcm.length);
  out[0] = kind;
  new DataView(out.buffer).setUint32(4, turn, true);
  out.set(pcm, AUDIO_HEADER_BYTES);
  return out;
}

export function parseAudioFrame(
  data: Uint8Array,
  expectedKind: number,
): { ok: true; frame: AudioFrame } | { ok: false; error: FrameError } {
  if (data.length < AUDIO_HEADER_BYTES) return { ok: false, error: "too_short" };
  if (
    data[0] !== expectedKind ||
    (expectedKind !== KIND_USER_AUDIO && expectedKind !== KIND_MODEL_AUDIO)
  ) return { ok: false, error: "bad_kind" };
  if (data[1] !== 0) return { ok: false, error: "bad_flags" };
  if (data[2] !== 0 || data[3] !== 0) return { ok: false, error: "bad_reserved" };
  const turn = new DataView(data.buffer, data.byteOffset, data.byteLength)
    .getUint32(4, true);
  if (turn === 0) return { ok: false, error: "zero_turn" };
  const pcmBytes = data.length - AUDIO_HEADER_BYTES;
  if (pcmBytes === 0) return { ok: false, error: "empty_payload" };
  if (pcmBytes % 2 !== 0) return { ok: false, error: "odd_payload" };
  const limit = expectedKind === KIND_USER_AUDIO
    ? MAX_UP_PCM_BYTES
    : MAX_DOWN_PCM_BYTES;
  if (pcmBytes > limit) return { ok: false, error: "oversize" };
  return {
    ok: true,
    frame: { kind: data[0], turn, pcm: data.subarray(AUDIO_HEADER_BYTES) },
  };
}

// --- device -> gateway ---------------------------------------------------------

export type DeviceMessage =
  | {
    t: "hello";
    proto: number;
    fw: string;
    in: string;
    out: string;
    maxDown: number;
    credit: number;
    // The device's saved activity selection; absent = the configured default.
    activity?: string;
  }
  | { t: "activity_select"; activity: string }
  | { t: "turn_start"; turn: number }
  | { t: "turn_end"; turn: number; frames: number; bytes: number }
  | { t: "cancel"; turn: number }
  | { t: "credit"; bytes: number }
  | { t: "ping"; ts: number }
  | { t: "unknown"; type: string };

export type ControlError =
  | "too_long"
  | "malformed"
  | "missing_type"
  | "missing_field"
  | "bad_value"
  | "bad_proto"
  | "bad_format";

export type ParseResult =
  | { ok: true; msg: DeviceMessage }
  | { ok: false; error: ControlError };

const FIRMWARE_RE = /^[A-Za-z0-9._-]{1,24}$/;
// A canonical lowercase UUID, exactly as the device validates it.
export const ACTIVITY_ID_RE = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/;
export const MAX_ACTIVITIES = 8;
export const MAX_ACTIVITY_TITLE_BYTES = 48;
export type ActivityMode = "push_to_talk" | "free_conversation";

function fail(error: ControlError): ParseResult {
  return { ok: false, error };
}

export function parseDeviceMessage(text: string): ParseResult {
  if (utf8Length(text) > MAX_CONTROL_BYTES) return fail("too_long");
  let value: unknown;
  try {
    value = JSON.parse(text);
  } catch {
    return fail("malformed");
  }
  if (value === null || typeof value !== "object" || Array.isArray(value)) {
    return fail("malformed");
  }
  const obj = value as Record<string, unknown>;
  // A flat object only, exactly as the device parser accepts.
  for (const v of Object.values(obj)) {
    if (v === null || typeof v === "object") return fail("malformed");
  }
  if (!("t" in obj)) return fail("missing_type");
  const t = obj.t;
  if (typeof t !== "string") return fail("bad_value");

  const need = (...keys: string[]): boolean => keys.every((k) => k in obj);

  switch (t) {
    case "hello": {
      if (!need("proto", "fw", "in", "out", "maxDown", "credit")) {
        return fail("missing_field");
      }
      if (obj.proto !== PROTOCOL_VERSION) return fail("bad_proto");
      if (typeof obj.fw !== "string" || !FIRMWARE_RE.test(obj.fw)) {
        return fail("bad_value");
      }
      if (obj.in !== FORMAT_UP || obj.out !== FORMAT_DOWN) {
        return fail("bad_format");
      }
      if (
        !isU32(obj.maxDown) || obj.maxDown < 2 || obj.maxDown % 2 !== 0 ||
        obj.maxDown > MAX_DOWN_PCM_BYTES
      ) return fail("bad_value");
      if (!isU32(obj.credit)) return fail("bad_value");
      if ("activity" in obj && (typeof obj.activity !== "string" || !ACTIVITY_ID_RE.test(obj.activity))) {
        return fail("bad_value");
      }
      return {
        ok: true,
        msg: {
          t,
          proto: obj.proto,
          fw: obj.fw,
          in: obj.in,
          out: obj.out,
          maxDown: obj.maxDown,
          credit: obj.credit,
          ...(typeof obj.activity === "string" ? { activity: obj.activity } : {}),
        },
      };
    }
    case "activity_select": {
      if (!need("activity")) return fail("missing_field");
      if (typeof obj.activity !== "string" || !ACTIVITY_ID_RE.test(obj.activity)) {
        return fail("bad_value");
      }
      return { ok: true, msg: { t, activity: obj.activity } };
    }
    case "turn_start":
    case "cancel": {
      if (!need("turn")) return fail("missing_field");
      if (!isValidTurn(obj.turn)) return fail("bad_value");
      return { ok: true, msg: { t, turn: obj.turn } };
    }
    case "turn_end": {
      if (!need("turn", "frames", "bytes")) return fail("missing_field");
      if (!isValidTurn(obj.turn) || !isU32(obj.frames) || !isU32(obj.bytes)) {
        return fail("bad_value");
      }
      return {
        ok: true,
        msg: { t, turn: obj.turn, frames: obj.frames, bytes: obj.bytes },
      };
    }
    case "credit": {
      if (!need("bytes")) return fail("missing_field");
      if (!isU32(obj.bytes)) return fail("bad_value");
      return { ok: true, msg: { t, bytes: obj.bytes } };
    }
    case "ping": {
      if (!need("ts")) return fail("missing_field");
      if (!isU32(obj.ts)) return fail("bad_value");
      return { ok: true, msg: { t, ts: obj.ts } };
    }
    default:
      // Forward compatibility: ignored and counted by the caller.
      return { ok: true, msg: { t: "unknown", type: t.slice(0, 32) } };
  }
}

// --- gateway -> device -----------------------------------------------------------
//
// Field lengths match the device's fixed buffers (GatewayProtocol.h), and the
// character sets exclude anything that would need a JSON escape the device
// parser does not accept.

const ID_RE = /^[A-Za-z0-9_-]{1,39}$/;
const CODE_RE = /^[a-z_]{1,31}$/;
const REASON_RE = /^[a-z_]{1,15}$/;

function finish(message: Record<string, unknown>): string {
  const text = JSON.stringify(message);
  if (utf8Length(text) > MAX_CONTROL_BYTES) {
    throw new RangeError("control frame too long");
  }
  return text;
}

export function encodeReady(session: string, activity: string): string {
  if (!ID_RE.test(session) || !ID_RE.test(activity)) {
    throw new RangeError("invalid session or activity id");
  }
  return finish({ t: "ready", session, activity, out: FORMAT_DOWN });
}

export function encodeSpeechStart(turn: number): string {
  if (!isValidTurn(turn)) throw new RangeError("invalid turn id");
  return finish({ t: "speech_start", turn, fmt: FORMAT_DOWN });
}

export function encodeTurnComplete(
  turn: number,
  frames: number,
  bytes: number,
): string {
  if (!isValidTurn(turn) || !isU32(frames) || !isU32(bytes)) {
    throw new RangeError("invalid turn_complete");
  }
  return finish({ t: "turn_complete", turn, frames, bytes });
}

export function encodeInterrupted(turn: number): string {
  if (!isValidTurn(turn)) throw new RangeError("invalid turn id");
  return finish({ t: "interrupted", turn });
}

export function encodeError(code: string, retry: boolean, turn?: number): string {
  if (!CODE_RE.test(code)) throw new RangeError("invalid error code");
  if (turn !== undefined) {
    if (!isValidTurn(turn)) throw new RangeError("invalid turn id");
    return finish({ t: "error", code, retry, turn });
  }
  return finish({ t: "error", code, retry });
}

export function encodeSessionEnd(reason: string): string {
  if (!REASON_RE.test(reason)) throw new RangeError("invalid reason");
  return finish({ t: "session_end", reason });
}

// A display title the device can store and parse: no quote, backslash or
// control character (the device parser takes no escapes), whitespace
// collapsed, at most MAX_ACTIVITY_TITLE_BYTES of UTF-8 cut on a character
// boundary, never empty.
export function safeActivityTitle(title: string): string {
  const cleaned = title.replace(/["\\ -]/g, " ").replace(/\s+/g, " ").trim();
  let out = "";
  for (const ch of cleaned) {
    if (utf8Length(out + ch) > MAX_ACTIVITY_TITLE_BYTES) break;
    out += ch;
  }
  out = out.trim();
  return out.length > 0 ? out : "Activitate";
}

export function encodeActivityList(
  index: number,
  count: number,
  activity: string,
  title: string,
  mode: ActivityMode,
  current: boolean,
): string {
  if (!isU32(count) || count < 1 || count > MAX_ACTIVITIES || !isU32(index) || index >= count) {
    throw new RangeError("invalid activity list position");
  }
  if (!ACTIVITY_ID_RE.test(activity)) throw new RangeError("invalid activity id");
  if (safeActivityTitle(title) !== title) throw new RangeError("unsafe activity title");
  if (mode !== "push_to_talk" && mode !== "free_conversation") throw new RangeError("invalid mode");
  return finish({ t: "activity_list", index, count, activity, title, mode, current });
}

export function encodeActivitySelected(activity: string): string {
  if (!ACTIVITY_ID_RE.test(activity)) throw new RangeError("invalid activity id");
  return finish({ t: "activity_selected", activity });
}

export function encodeActivitySelectError(code: string, activity?: string): string {
  if (!CODE_RE.test(code)) throw new RangeError("invalid error code");
  if (activity === undefined) return finish({ t: "activity_select_error", code });
  if (!ACTIVITY_ID_RE.test(activity)) throw new RangeError("invalid activity id");
  return finish({ t: "activity_select_error", code, activity });
}

export function encodePong(ts: number): string {
  if (!isU32(ts)) throw new RangeError("invalid ts");
  return finish({ t: "pong", ts });
}
