// The LAN-development fake Gemini (Step 6.2): configuration guard rails, the
// connector's three behaviours, and a real DeviceSession reaching `ready`
// through it.

import { assert, assertEquals } from "./assert.ts";
import { DEV_ACTIVITY, fakeGeminiConnector, parseFakeSetup } from "../src/fake_gemini.ts";
import { GeminiEvent } from "../src/gemini.ts";
import { createLogger } from "../src/log.ts";
import { sha256 } from "../src/registry.ts";
import { createHandler, loadConfig } from "../src/server.ts";
import { DeviceSession } from "../src/session.ts";
import { FakeDevice, FakeTimers, flush } from "./session_harness.ts";

const TOKEN = "T".repeat(43);
const HELLO =
  `{"t":"hello","proto":1,"fw":"core2-6.2","in":"s16le/16000/1","out":"s16le/24000/1","maxDown":1920,"credit":192000}`;

function throws(fn: () => unknown): boolean {
  try {
    fn();
    return false;
  } catch {
    return true;
  }
}

Deno.test("fake mode: no mint secret or Supabase needed, refused where it could be deployed", () => {
  const base: Record<string, string> = { GEMINI_MODE: "fake", TTH_DEVICES: "{}" };
  const cfg = loadConfig((n) => base[n]);
  assertEquals(cfg.geminiMode, "fake");
  assertEquals(cfg.mintSecret, "");
  assertEquals(cfg.supabaseUrl, "");
  assertEquals(cfg.fakeSetup, "ok");
  assertEquals(cfg.trustProxy, false);

  assert(throws(() => loadConfig((n) => ({ ...base, K_SERVICE: "tth-gateway" })[n])), "Cloud Run");
  assert(throws(() => loadConfig((n) => ({ ...base, TRUST_PROXY: "1" })[n])), "behind a proxy");
  assert(throws(() => loadConfig((n) => ({ ...base, GEMINI_MODE: "bogus" })[n])), "bad mode");
  assert(throws(() => loadConfig((n) => ({ ...base, FAKE_GEMINI_SETUP: "maybe" })[n])), "bad setup");
  // The default is still the live mode, which needs every secret.
  assert(throws(() => loadConfig((n) => ({ TTH_DEVICES: "{}" } as Record<string, string>)[n])));
});

Deno.test("parseFakeSetup accepts ok, fail and never only", () => {
  assertEquals(parseFakeSetup(undefined), "ok");
  assertEquals(parseFakeSetup(""), "ok");
  assertEquals(parseFakeSetup("never"), "never");
  assertEquals(parseFakeSetup("fail"), "fail");
  assert(throws(() => parseFakeSetup("OK")));
});

Deno.test("fake connector: ok completes setup, never does not, fail refuses", async () => {
  const scheduled: Array<() => void> = [];
  const schedule = (fn: () => void) => {
    scheduled.push(fn);
  };
  const events: GeminiEvent[] = [];
  const handlers = { onEvent: (e: GeminiEvent) => events.push(e), onClose: () => {} };

  await fakeGeminiConnector({ setup: "ok", schedule })("t", "{}", handlers);
  assertEquals(events.length, 0);
  scheduled.splice(0).forEach((fn) => fn());
  assertEquals(events.length, 1);
  assertEquals(events[0].type, "setup_complete");

  const closedEarly = await fakeGeminiConnector({ setup: "ok", schedule })("t", "{}", handlers);
  closedEarly.close();
  scheduled.splice(0).forEach((fn) => fn());
  assertEquals(events.length, 1);

  await fakeGeminiConnector({ setup: "never", schedule })("t", "{}", handlers);
  assertEquals(scheduled.length, 0);

  let refused = false;
  await fakeGeminiConnector({ setup: "fail", schedule })("t", "{}", handlers).catch(() => {
    refused = true;
  });
  assert(refused, "fail must refuse the connection");
});

function session(setup: "ok" | "fail") {
  const device = new FakeDevice();
  const s = new DeviceSession({
    deviceId: "core2-01",
    sessionId: "s-1",
    resolved: { activity: DEV_ACTIVITY, convertedFromFreeConversation: false },
    systemInstruction: "SYS",
    device,
    mintToken: () => Promise.resolve("fake-gemini-token"),
    connectGemini: fakeGeminiConnector({ setup, schedule: (fn) => setTimeout(fn, 0) }),
    timers: new FakeTimers(),
    log: createLogger(() => {}, () => "T"),
  });
  return { device, s };
}

Deno.test("a device session reaches ready through the fake Gemini", async () => {
  const { device, s } = session("ok");
  s.onText(HELLO);
  await flush();
  await flush();
  assertEquals(device.take(), ["ready"]);
  assertEquals(s.phase, "ready");
  // Keepalive works once ready.
  s.onText(`{"t":"ping","ts":42}`);
  assertEquals(device.take(), ["pong"]);
});

Deno.test("a failing fake Gemini gives the device a retryable error and closes", async () => {
  const { device, s } = session("fail");
  s.onText(HELLO);
  await flush();
  await flush();
  const sent = device.sent;
  assertEquals(sent.length, 1);
  assert(sent[0].kind === "text" && sent[0].msg.t === "error", "an error message");
  if (sent[0].kind === "text") {
    assertEquals(sent[0].msg.code, "gemini_unavailable");
    assertEquals(sent[0].msg.retry, true);
  }
  assertEquals(device.closed?.code, 1011);
});

Deno.test("fake mode without Supabase serves the dev activity and never fetches", async () => {
  const digest = [...await sha256(new TextEncoder().encode(TOKEN))]
    .map((b) => b.toString(16).padStart(2, "0")).join("");
  const cfg = loadConfig((n) =>
    ({
      GEMINI_MODE: "fake",
      TTH_DEVICES: JSON.stringify({
        "core2-01": { token_sha256: digest, activity_id: null, enabled: true },
      }),
    } as Record<string, string>)[n]
  );
  let fetched = 0;
  let upgraded = 0;
  const handler = createHandler({
    config: cfg,
    log: createLogger(() => {}, () => "T"),
    connectGemini: fakeGeminiConnector({ setup: "never" }),
    fetchFn: (() => {
      fetched++;
      return Promise.reject(new Error("no network in fake mode"));
    }) as typeof fetch,
    now: () => 0,
    upgrade: (() => {
      upgraded++;
      return { socket: {} as WebSocket, response: new Response(null, { status: 200 }) };
    }) as unknown as typeof Deno.upgradeWebSocket,
  });
  const info = { remoteAddr: { hostname: "10.0.0.1", port: 1, transport: "tcp" } } as unknown as Deno.ServeHandlerInfo;
  const res = await handler(
    new Request("https://gw.test/v1/ws", {
      headers: {
        upgrade: "websocket",
        "sec-websocket-protocol": "tth.v1",
        "x-tth-device": "core2-01",
        authorization: `Bearer ${TOKEN}`,
      },
    }),
    info,
  );
  assertEquals(res.status, 200);
  assertEquals(upgraded, 1);
  assertEquals(fetched, 0);
});
