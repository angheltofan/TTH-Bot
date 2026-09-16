// Pre-upgrade checks of the HTTP entry: nothing reaches a WebSocket without
// a valid device token, within rate limits, for a valid activity.

import { assert, assertEquals } from "./assert.ts";
import { createLogger } from "../src/log.ts";
import { parseRegistry, sha256 } from "../src/registry.ts";
import { createHandler, GatewayConfig, loadConfig } from "../src/server.ts";

const TOKEN = "T".repeat(43);

async function config(activityId: string | null = null): Promise<GatewayConfig> {
  const digest = [...await sha256(new TextEncoder().encode(TOKEN))]
    .map((b) => b.toString(16).padStart(2, "0")).join("");
  return {
    supabaseUrl: "https://example.test",
    publishableKey: "sb_publishable_test",
    mintFunctionUrl: "https://example.test/functions/v1/gateway-gemini-token",
    mintSecret: "x".repeat(32),
    registry: parseRegistry(JSON.stringify({
      "core2-01": { token_sha256: digest, activity_id: activityId, enabled: true },
    })),
    port: 0,
    trustProxy: false,
  };
}

const ACTIVITIES = [{
  id: "0f8fad5b-d9cb-469f-a165-70867728950e",
  title: "Baschet",
  type: "lesson",
  prompt: "Activitate.",
  participants: ["Maria"],
  interaction_mode: "push_to_talk",
  enabled: true,
  sort_order: 0,
}];

function setup(cfg: GatewayConfig, rows: unknown[] = ACTIVITIES) {
  const logs: string[] = [];
  let upgraded = 0;
  const fetchFn = (() =>
    Promise.resolve(new Response(JSON.stringify(rows), { status: 200 }))) as typeof fetch;
  const handler = createHandler({
    config: cfg,
    log: createLogger((l) => logs.push(l), () => "T"),
    connectGemini: () => Promise.reject(new Error("not in these tests")),
    fetchFn,
    now: () => 0,
    upgrade: (() => {
      upgraded++;
      // A stand-in socket: these tests stop at the upgrade decision.
      return { socket: {} as WebSocket, response: new Response(null, { status: 200 }) };
    }) as unknown as typeof Deno.upgradeWebSocket,
  });
  const info = { remoteAddr: { hostname: "10.0.0.1", port: 1, transport: "tcp" } } as unknown as Deno.ServeHandlerInfo;
  const call = (headers: Record<string, string>, path = "/v1/ws") =>
    handler(new Request(`https://gw.test${path}`, { headers }), info);
  return { call, logs, upgradedCount: () => upgraded };
}

const WS = { upgrade: "websocket", "sec-websocket-protocol": "tth.v1" };

Deno.test("health endpoint", async () => {
  const s = setup(await config());
  assertEquals((await s.call({}, "/healthz")).status, 200);
});

Deno.test("non-WebSocket and wrong subprotocol requests are refused", async () => {
  const s = setup(await config());
  assertEquals((await s.call({})).status, 426);
  assertEquals((await s.call({ upgrade: "websocket" })).status, 400);
  assertEquals(s.upgradedCount(), 0);
});

Deno.test("missing or wrong device credentials: 401, no upgrade", async () => {
  const s = setup(await config());
  assertEquals((await s.call({ ...WS })).status, 401);
  assertEquals(
    (await s.call({ ...WS, "x-tth-device": "core2-01", authorization: `Bearer ${"X".repeat(43)}` })).status,
    401,
  );
  assertEquals(s.upgradedCount(), 0);
  assert(!s.logs.join("").includes("X".repeat(43)), "a token reached the log");
});

Deno.test("repeated auth failures block the source", async () => {
  const s = setup(await config());
  for (let i = 0; i < 5; i++) await s.call({ ...WS });
  const res = await s.call({ ...WS, "x-tth-device": "core2-01", authorization: `Bearer ${TOKEN}` });
  assertEquals(res.status, 429);
});

Deno.test("a valid device is upgraded", async () => {
  const s = setup(await config());
  const res = await s.call({ ...WS, "x-tth-device": "core2-01", authorization: `Bearer ${TOKEN}` });
  assertEquals(res.status, 200);
  assertEquals(s.upgradedCount(), 1);
});

Deno.test("per-device connection rate is limited", async () => {
  const s = setup(await config());
  const headers = { ...WS, "x-tth-device": "core2-01", authorization: `Bearer ${TOKEN}` };
  const statuses: number[] = [];
  for (let i = 0; i < 7; i++) statuses.push((await s.call(headers)).status);
  assertEquals(statuses.filter((x) => x === 200).length, 6);
  assertEquals(statuses[6], 429);
});

Deno.test("an activity over the limits is refused before upgrading", async () => {
  const rows = [{ ...ACTIVITIES[0], participants: Array(13).fill("Ana") }];
  const s = setup(await config(), rows);
  const res = await s.call({ ...WS, "x-tth-device": "core2-01", authorization: `Bearer ${TOKEN}` });
  assertEquals(res.status, 503);
  assertEquals(s.upgradedCount(), 0);
});

Deno.test("a registry activity that does not exist is refused, not substituted", async () => {
  const s = setup(await config("11111111-1111-4111-8111-111111111111"));
  const res = await s.call({ ...WS, "x-tth-device": "core2-01", authorization: `Bearer ${TOKEN}` });
  assertEquals(res.status, 503);
});

Deno.test("configuration: required settings and a strong mint secret", () => {
  const base: Record<string, string> = {
    SUPABASE_URL: "https://x.supabase.co/",
    SUPABASE_PUBLISHABLE_KEY: "k",
    GATEWAY_MINT_SECRET: "s".repeat(32),
    TTH_DEVICES: "{}",
  };
  const cfg = loadConfig((n) => base[n]);
  assertEquals(cfg.mintFunctionUrl, "https://x.supabase.co/functions/v1/gateway-gemini-token");
  let threw = false;
  try {
    loadConfig((n) => ({ ...base, GATEWAY_MINT_SECRET: "short" })[n]);
  } catch {
    threw = true;
  }
  assert(threw, "a short secret must be refused");
});
