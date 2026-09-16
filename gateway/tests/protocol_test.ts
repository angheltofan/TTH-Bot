// tth.v1 codec, gateway side. The message strings are the SAME byte-exact
// vectors as firmware/core2/test/test_gateway_protocol.

import { assert, assertEquals, assertThrows } from "./assert.ts";
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
  MAX_CONTROL_BYTES,
  parseAudioFrame,
  parseDeviceMessage,
} from "../src/protocol.ts";

Deno.test("the header carries a uint32 little-endian turn", () => {
  const frame = encodeAudioFrame(KIND_USER_AUDIO, 0x12345678, new Uint8Array(4));
  assertEquals([...frame.subarray(0, 8)], [0x01, 0, 0, 0, 0x78, 0x56, 0x34, 0x12]);
  const parsed = parseAudioFrame(frame, KIND_USER_AUDIO);
  assert(parsed.ok);
  assertEquals(parsed.frame.turn, 0x12345678);
  assertEquals(parsed.frame.pcm.length, 4);
});

Deno.test("the largest turn id round-trips; 0 is rejected", () => {
  const frame = encodeAudioFrame(KIND_MODEL_AUDIO, 0xffffffff, new Uint8Array(2));
  const parsed = parseAudioFrame(frame, KIND_MODEL_AUDIO);
  assert(parsed.ok);
  assertEquals(parsed.frame.turn, 0xffffffff);
  assertThrows(() => encodeAudioFrame(KIND_USER_AUDIO, 0, new Uint8Array(2)));
  const zero = new Uint8Array(10);
  zero[0] = KIND_USER_AUDIO;
  assertEquals(parseAudioFrame(zero, KIND_USER_AUDIO), { ok: false, error: "zero_turn" });
});

Deno.test("malformed binary frames are rejected", () => {
  const good = encodeAudioFrame(KIND_MODEL_AUDIO, 7, new Uint8Array(1920));
  assert(parseAudioFrame(good, KIND_MODEL_AUDIO).ok);
  assertEquals(parseAudioFrame(good.subarray(0, 7), KIND_MODEL_AUDIO), {
    ok: false,
    error: "too_short",
  });
  assertEquals(parseAudioFrame(good.subarray(0, 8), KIND_MODEL_AUDIO), {
    ok: false,
    error: "empty_payload",
  });
  assertEquals(parseAudioFrame(good.subarray(0, 11), KIND_MODEL_AUDIO), {
    ok: false,
    error: "odd_payload",
  });
  assertEquals(parseAudioFrame(good, KIND_USER_AUDIO), { ok: false, error: "bad_kind" });
  const oversize = new Uint8Array(8 + 1922);
  oversize.set(good.subarray(0, 8));
  assertEquals(parseAudioFrame(oversize, KIND_MODEL_AUDIO), {
    ok: false,
    error: "oversize",
  });
  const up = new Uint8Array(8 + 642);
  up.set(encodeAudioFrame(KIND_USER_AUDIO, 1, new Uint8Array(2)).subarray(0, 8));
  assertEquals(parseAudioFrame(up, KIND_USER_AUDIO), { ok: false, error: "oversize" });
  const flags = good.slice();
  flags[1] = 1;
  assertEquals(parseAudioFrame(flags, KIND_MODEL_AUDIO), { ok: false, error: "bad_flags" });
  const reserved = good.slice();
  reserved[3] = 1;
  assertEquals(parseAudioFrame(reserved, KIND_MODEL_AUDIO), {
    ok: false,
    error: "bad_reserved",
  });
});

Deno.test("device messages parse (the C++ encoder's vectors)", () => {
  assertEquals(
    parseDeviceMessage(
      '{"t":"hello","proto":1,"fw":"core2-6.0","in":"s16le/16000/1","out":"s16le/24000/1","maxDown":1920,"credit":192000}',
    ),
    {
      ok: true,
      msg: {
        t: "hello",
        proto: 1,
        fw: "core2-6.0",
        in: "s16le/16000/1",
        out: "s16le/24000/1",
        maxDown: 1920,
        credit: 192000,
      },
    },
  );
  assertEquals(parseDeviceMessage('{"t":"turn_start","turn":7}'), {
    ok: true,
    msg: { t: "turn_start", turn: 7 },
  });
  assertEquals(parseDeviceMessage('{"t":"turn_start","turn":4294967295}'), {
    ok: true,
    msg: { t: "turn_start", turn: 4294967295 },
  });
  assertEquals(parseDeviceMessage('{"t":"turn_end","turn":7,"frames":12,"bytes":7680}'), {
    ok: true,
    msg: { t: "turn_end", turn: 7, frames: 12, bytes: 7680 },
  });
  assertEquals(parseDeviceMessage('{"t":"cancel","turn":7}'), {
    ok: true,
    msg: { t: "cancel", turn: 7 },
  });
  assertEquals(parseDeviceMessage('{"t":"credit","bytes":3840}'), {
    ok: true,
    msg: { t: "credit", bytes: 3840 },
  });
  assertEquals(parseDeviceMessage('{"t":"ping","ts":123}'), {
    ok: true,
    msg: { t: "ping", ts: 123 },
  });
});

Deno.test("device messages are validated strictly", () => {
  const err = (text: string) => {
    const r = parseDeviceMessage(text);
    return r.ok ? "ok" : r.error;
  };
  assertEquals(err("nope"), "malformed");
  assertEquals(err("[]"), "malformed");
  assertEquals(err('{"t":"ping","ts":{"a":1}}'), "malformed");
  assertEquals(err('{"t":"ping","ts":null}'), "malformed");
  assertEquals(err('{"ts":1}'), "missing_type");
  assertEquals(err('{"t":1}'), "bad_value");
  assertEquals(err('{"t":"turn_start","turn":0}'), "bad_value");
  assertEquals(err('{"t":"turn_start","turn":4294967296}'), "bad_value");
  assertEquals(err('{"t":"turn_start","turn":1.5}'), "bad_value");
  assertEquals(err('{"t":"turn_end","turn":1,"frames":1}'), "missing_field");
  assertEquals(err('{"t":"credit","bytes":-1}'), "bad_value");
  assertEquals(
    err('{"t":"hello","proto":2,"fw":"x","in":"s16le/16000/1","out":"s16le/24000/1","maxDown":1920,"credit":1}'),
    "bad_proto",
  );
  assertEquals(
    err('{"t":"hello","proto":1,"fw":"x","in":"s16le/24000/1","out":"s16le/24000/1","maxDown":1920,"credit":1}'),
    "bad_format",
  );
  assertEquals(
    err('{"t":"hello","proto":1,"fw":"bad fw","in":"s16le/16000/1","out":"s16le/24000/1","maxDown":1920,"credit":1}'),
    "bad_value",
  );
  assertEquals(err(`{"t":"ping","ts":1,"pad":"${"x".repeat(MAX_CONTROL_BYTES)}"}`), "too_long");
});

Deno.test("an unknown device message type is ignored, not an error", () => {
  assertEquals(parseDeviceMessage('{"t":"future_thing","x":1}'), {
    ok: true,
    msg: { t: "unknown", type: "future_thing" },
  });
});

Deno.test("gateway messages encode byte-exactly (the C++ parser's vectors)", () => {
  assertEquals(
    encodeReady("s-1", "a-1"),
    '{"t":"ready","session":"s-1","activity":"a-1","out":"s16le/24000/1"}',
  );
  assertEquals(encodeSpeechStart(7), '{"t":"speech_start","turn":7,"fmt":"s16le/24000/1"}');
  assertEquals(
    encodeTurnComplete(7, 30, 57600),
    '{"t":"turn_complete","turn":7,"frames":30,"bytes":57600}',
  );
  assertEquals(encodeInterrupted(7), '{"t":"interrupted","turn":7}');
  assertEquals(
    encodeError("no_response", true, 7),
    '{"t":"error","code":"no_response","retry":true,"turn":7}',
  );
  assertEquals(encodeError("auth", false), '{"t":"error","code":"auth","retry":false}');
  assertEquals(encodeSessionEnd("max_age"), '{"t":"session_end","reason":"max_age"}');
  assertEquals(encodePong(4294967295), '{"t":"pong","ts":4294967295}');
});

Deno.test("gateway encoders refuse values the device could not hold", () => {
  assertThrows(() => encodeReady("s 1", "a"));
  assertThrows(() => encodeReady("s", "x".repeat(40)));
  assertThrows(() => encodeError("Bad-Code", true));
  assertThrows(() => encodeError("x".repeat(32), true));
  assertThrows(() => encodeSessionEnd("far_too_long_reason"));
  assertThrows(() => encodeSpeechStart(0));
});
