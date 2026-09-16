// The Gemini Live WebSocket endpoint for ephemeral tokens
// (https://ai.google.dev/api/live): tokens minted by gateway-gemini-token
// work only with GenerativeService.BidiGenerateContentConstrained, passed as
// the access_token query parameter.

import { assert, assertEquals } from "./assert.ts";
import { DEV_ACTIVITY } from "../src/fake_gemini.ts";
import {
  GEMINI_WS_URL,
  GEMINI_WS_URL_API_KEY,
  GeminiConnector,
  liveSocketUrl,
  webSocketConnector,
} from "../src/gemini.ts";
import { createLogger } from "../src/log.ts";
import { DeviceSession } from "../src/session.ts";
import { FakeDevice, FakeTimers, flush } from "./session_harness.ts";

// A marker, not a token shape: the secret scan must never see a real token prefix.
const TOKEN = "TOKEN-SENTINEL-abc+/=";

Deno.test("ephemeral-token sessions use BidiGenerateContentConstrained on v1beta", () => {
  assertEquals(
    GEMINI_WS_URL,
    "wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContentConstrained",
  );
  const url = new URL(liveSocketUrl(TOKEN));
  assertEquals(url.protocol, "wss:");
  assertEquals(url.hostname, "generativelanguage.googleapis.com");
  assert(url.pathname.endsWith(".v1beta.GenerativeService.BidiGenerateContentConstrained"), "constrained method");
});

Deno.test("the token travels only as the access_token query parameter", () => {
  const url = new URL(liveSocketUrl(TOKEN));
  assertEquals([...url.searchParams.keys()].join(","), "access_token");
  assertEquals(url.searchParams.get("access_token"), TOKEN);
  assertEquals(url.searchParams.get("key"), null);
  assert(!url.pathname.includes("TOKEN-SENTINEL"), "not in the path");
});

Deno.test("the API-key endpoint is never used for ephemeral tokens", () => {
  const url = liveSocketUrl(TOKEN);
  assert(!url.startsWith(`${GEMINI_WS_URL_API_KEY}?`), "not the unconstrained method");
  // The unconstrained method remains only for a developer key (manual probe).
  assert(liveSocketUrl("dev-key", "key").startsWith(`${GEMINI_WS_URL_API_KEY}?key=`), "probe path");
});

Deno.test("a connection failure never carries the token in its error", async () => {
  const handlers = { onEvent: () => {}, onClose: () => {} };
  let message = "";
  try {
    // An invalid URL makes the WebSocket constructor throw synchronously; its
    // own message would include the URL with the token.
    await webSocketConnector("not a valid url")(TOKEN, "{}", handlers);
  } catch (e) {
    message = `${(e as Error).message} ${(e as Error).stack ?? ""}`;
  }
  assert(message.startsWith("gemini_connect_failed"), "a fixed error");
  assert(!message.includes("TOKEN-SENTINEL"), "no token in the error");
});

Deno.test("the minted token never reaches the session logs, on success or failure", async () => {
  for (const outcome of ["ok", "fail"] as const) {
    const logs: string[] = [];
    const tokensSeen: string[] = [];
    const connector: GeminiConnector = (token, _setup, handlers) => {
      tokensSeen.push(token);
      if (outcome === "fail") return Promise.reject(new Error("gemini_connect_failed"));
      // Like a real socket: the event arrives after the link is handed back.
      setTimeout(() => handlers.onEvent({ type: "setup_complete" }), 0);
      return Promise.resolve({ send: () => {}, close: () => {} });
    };
    const session = new DeviceSession({
      deviceId: "core2-01",
      sessionId: "s-1",
      resolved: { activity: DEV_ACTIVITY, convertedFromFreeConversation: false },
      systemInstruction: "SYS",
      device: new FakeDevice(),
      mintToken: () => Promise.resolve(TOKEN),
      connectGemini: connector,
      timers: new FakeTimers(),
      log: createLogger((line) => logs.push(line), () => "T"),
    });
    session.onText(
      `{"t":"hello","proto":1,"fw":"core2-6.3","in":"s16le/16000/1","out":"s16le/24000/1","maxDown":1920,"credit":192000}`,
    );
    await flush();
    await flush();
    await flush();
    session.close(1000, "test_done");
    assertEquals(tokensSeen, [TOKEN]);
    assert(logs.length > 0, "the session logged something");
    for (const line of logs) assert(!line.includes("TOKEN-SENTINEL"), `no token in: ${line.slice(0, 40)}`);
  }
});
