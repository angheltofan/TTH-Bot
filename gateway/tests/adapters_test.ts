// Activities, the Gemini setup/parse adapter, and the token client.

import { assert, assertEquals, assertRejects } from "./assert.ts";
import { Activity, parseActivityRow, resolveActivity } from "../src/activities.ts";
import {
  ACTIVITY_END,
  ACTIVITY_START,
  audioMessage,
  base64Decode,
  base64Encode,
  buildSetupMessage,
  parseServerMessage,
} from "../src/gemini.ts";
import {
  buildMintHeader,
  MINT_HEADER,
  mintGeminiToken,
  signMintAssertion,
} from "../src/token_client.ts";

// --- activities ---------------------------------------------------------------

function activity(id: string, mode: Activity["interactionMode"], sortOrder: number, enabled = true): Activity {
  return {
    id,
    title: id,
    type: "lesson",
    prompt: "p",
    participants: [],
    interactionMode: mode,
    enabled,
    sortOrder,
  };
}

Deno.test("activities: the first enabled push-to-talk activity is the default", () => {
  const list = [
    activity("free", "free_conversation", 0),
    activity("off", "push_to_talk", 0, false),
    activity("ptt-2", "push_to_talk", 2),
    activity("ptt-1", "push_to_talk", 1),
  ];
  assertEquals(resolveActivity(list, null), {
    activity: list[3],
    convertedFromFreeConversation: false,
  });
});

Deno.test("activities: free_conversation runs as push-to-talk, reported as converted", () => {
  const list = [activity("free", "free_conversation", 0)];
  assertEquals(resolveActivity(list, null)?.convertedFromFreeConversation, true);
  assertEquals(resolveActivity(list, "free")?.convertedFromFreeConversation, true);
});

Deno.test("activities: a device override that is missing or disabled is refused", () => {
  const list = [activity("a", "push_to_talk", 0), activity("b", "push_to_talk", 1, false)];
  assertEquals(resolveActivity(list, "b"), null);
  assertEquals(resolveActivity(list, "zzz"), null);
  assertEquals(resolveActivity(list, "a")?.activity.id, "a");
  assertEquals(resolveActivity([], null), null);
});

Deno.test("activities: rows are validated", () => {
  const row = {
    id: "x",
    title: "Baschet",
    type: "lesson",
    prompt: "p",
    participants: ["Maria"],
    interaction_mode: "push_to_talk",
    enabled: true,
    sort_order: 0,
  };
  assertEquals(parseActivityRow(row).participants, ["Maria"]);
  let threw = false;
  try {
    parseActivityRow({ ...row, interaction_mode: "telepathy" });
  } catch {
    threw = true;
  }
  assert(threw);
});

// --- Gemini ---------------------------------------------------------------------

Deno.test("gemini: the setup message equals the Flutter push-to-talk setup", () => {
  // Field for field, in the order the Dart map literal builds it.
  assertEquals(
    buildSetupMessage("SYS"),
    JSON.stringify({
      setup: {
        model: "models/gemini-3.1-flash-live-preview",
        generationConfig: {
          responseModalities: ["AUDIO"],
          speechConfig: { voiceConfig: { prebuiltVoiceConfig: { voiceName: "Puck" } } },
        },
        systemInstruction: { parts: [{ text: "SYS" }] },
        realtimeInputConfig: { automaticActivityDetection: { disabled: true } },
      },
    }),
  );
  assertEquals(ACTIVITY_START, '{"realtimeInput":{"activityStart":{}}}');
  assertEquals(ACTIVITY_END, '{"realtimeInput":{"activityEnd":{}}}');
});

Deno.test("gemini: user audio goes out as base64 PCM at 16 kHz", () => {
  const pcm = new Uint8Array([1, 2, 3, 4]);
  const msg = JSON.parse(audioMessage(pcm));
  assertEquals(msg.realtimeInput.audio.mimeType, "audio/pcm;rate=16000");
  assertEquals(base64Decode(msg.realtimeInput.audio.data), pcm);
  const big = new Uint8Array(100_000).map((_, i) => i & 0xff);
  assertEquals(base64Decode(base64Encode(big)), big);
});

Deno.test("gemini: server messages parse like the Flutter parser, plus goAway", () => {
  assertEquals(parseServerMessage({ setupComplete: {} }), [{ type: "setup_complete" }]);
  assertEquals(parseServerMessage({ goAway: { timeLeft: "10s" } }), [{ type: "go_away" }]);
  const audio = parseServerMessage({
    serverContent: {
      modelTurn: {
        parts: [{ inlineData: { mimeType: "audio/pcm;rate=24000", data: base64Encode(new Uint8Array([9, 8])) } }],
      },
      turnComplete: true,
    },
  });
  assertEquals(audio, [{ type: "audio", pcm: new Uint8Array([9, 8]) }, { type: "turn_complete" }]);
  assertEquals(parseServerMessage({ serverContent: { interrupted: true } }), [
    { type: "interrupted" },
  ]);
  // Audio in a format the device cannot play is reported, not forwarded.
  assertEquals(
    parseServerMessage({
      serverContent: {
        modelTurn: { parts: [{ inlineData: { mimeType: "audio/pcm;rate=16000", data: "AAA=" } }] },
      },
    }),
    [{ type: "error", message: "unexpected_audio_format" }],
  );
  assertEquals(parseServerMessage({ error: { message: "bad" } }), [
    { type: "error", message: "server_error" },
  ]);
});

// --- token client ------------------------------------------------------------------

// The SAME vector is asserted by the Supabase function's tests
// (supabase/functions/gateway-gemini-token/handler_test.ts), and was computed
// independently with Node's crypto.createHmac.
const VECTOR = {
  secret: "test-gateway-secret",
  ts: 1757600000000,
  nonce: "AAECAwQFBgcICQoLDA0ODw",
  sig: "tS3Bh2_7tIG4Kb1iOMUJc5l58sBNjJzCERiJS7TqVjM",
};

Deno.test("token client: the HMAC assertion matches the shared vector", async () => {
  assertEquals(await signMintAssertion(VECTOR.secret, VECTOR.ts, VECTOR.nonce), VECTOR.sig);
  const header = await buildMintHeader(
    VECTOR.secret,
    VECTOR.ts,
    new Uint8Array([...Array(16).keys()]),
  );
  assertEquals(header, `ts=${VECTOR.ts},nonce=${VECTOR.nonce},sig=${VECTOR.sig}`);
});

Deno.test("token client: posts the assertion, never the secret", async () => {
  let seen: Request | null = null;
  const fetchFn = ((input: Request | URL | string, init?: RequestInit) => {
    seen = new Request(input, init);
    return Promise.resolve(new Response('{"token":"ephemeral-1"}', { status: 200 }));
  }) as typeof fetch;
  const token = await mintGeminiToken({
    fetchFn,
    functionUrl: "https://example.test/functions/v1/gateway-gemini-token",
    publishableKey: "sb_publishable_x",
    secret: "super-secret-value",
    nowMs: 1,
  });
  assertEquals(token, "ephemeral-1");
  const req = seen as unknown as Request;
  assertEquals(req.method, "POST");
  const auth = req.headers.get(MINT_HEADER) ?? "";
  assert(/^ts=1,nonce=[A-Za-z0-9_-]{22},sig=[A-Za-z0-9_-]{43}$/.test(auth));
  const everything = [...req.headers.entries()].join(";") + (await req.text());
  assert(!everything.includes("super-secret-value"), "the secret was transmitted");
});

Deno.test("token client: failures are reported, not retried silently", async () => {
  const fetchFn = (() => Promise.resolve(new Response("{}", { status: 401 }))) as typeof fetch;
  await assertRejects(() =>
    mintGeminiToken({ fetchFn, functionUrl: "u", publishableKey: "k", secret: "s", nowMs: 1 })
  );
  const bad = (() => Promise.resolve(new Response('{"nope":1}', { status: 200 }))) as typeof fetch;
  await assertRejects(() =>
    mintGeminiToken({ fetchFn: bad, functionUrl: "u", publishableKey: "k", secret: "s", nowMs: 1 })
  );
});
