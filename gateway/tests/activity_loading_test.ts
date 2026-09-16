// V5: per-device activity loading from Supabase, prompt construction, and
// what happens on every failure path — through the real HTTP handler, the
// real DeviceSession and a capturing Gemini connector.
//
// Markers stand in for the prompt, the title and a child's name, so the
// redaction checks can prove none of them reaches a log line.

import { assert, assertEquals, assertThrows } from "./assert.ts";
import {
  fetchActivityById,
  MAX_ACTIVITY_RESPONSE_CHARS,
  parseActivityRow,
} from "../src/activities.ts";
import { GeminiConnector, GeminiHandlers } from "../src/gemini.ts";
import { createLogger } from "../src/log.ts";
import { composeSystemInstruction } from "../src/prompt.ts";
import { parseRegistry, sha256 } from "../src/registry.ts";
import { createHandler, GatewayConfig } from "../src/server.ts";
import { flush } from "./session_harness.ts";

const TOKEN = "T".repeat(43);
const ID = "0f8fad5b-d9cb-469f-a165-70867728950e";
const PROMPT = "Activitate PROMPTSENTINEL despre baschet.";
const TITLE = "TITLESENTINEL";
const CHILD = "Sentinelia";
const PUBLISHABLE = "sb_publishable_test";
const HELLO =
  `{"t":"hello","proto":1,"fw":"core2-6.3","in":"s16le/16000/1","out":"s16le/24000/1","maxDown":1920,"credit":192000}`;
const SENTINELS = ["PROMPTSENTINEL", TITLE, CHILD];

function row(overrides: Record<string, unknown> = {}): Record<string, unknown> {
  return {
    id: ID,
    title: TITLE,
    type: "lesson",
    prompt: PROMPT,
    participants: [CHILD, "Ana"],
    interaction_mode: "push_to_talk",
    enabled: true,
    sort_order: 0,
    ...overrides,
  };
}

async function config(activityId: string | null = ID): Promise<GatewayConfig> {
  const digest = [...await sha256(new TextEncoder().encode(TOKEN))]
    .map((b) => b.toString(16).padStart(2, "0")).join("");
  return {
    supabaseUrl: "https://example.test",
    publishableKey: PUBLISHABLE,
    mintFunctionUrl: "",
    mintSecret: "",
    registry: parseRegistry(JSON.stringify({
      "core2-01": { token_sha256: digest, activity_id: activityId, enabled: true },
    })),
    port: 0,
    trustProxy: false,
    geminiMode: "fake",
    fakeSetup: "ok",
  };
}

interface Db {
  rows: Record<string, unknown>[];
  fail?: "http" | "network";
}

function fakeSocket() {
  return {
    binaryType: "arraybuffer",
    bufferedAmount: 0,
    sent: [] as string[],
    send(data: unknown) {
      this.sent.push(String(data));
    },
    close() {},
    onopen: null as ((e: Event) => void) | null,
    onmessage: null as ((e: MessageEvent) => void) | null,
    onclose: null as ((e: CloseEvent) => void) | null,
    onerror: null as ((e: Event) => void) | null,
  };
}

function gateway(cfg: GatewayConfig, db: Db) {
  const logs: string[] = [];
  const requests: { url: URL; headers: Headers }[] = [];
  const setups: string[] = [];
  const links: GeminiHandlers[] = [];
  const sockets: ReturnType<typeof fakeSocket>[] = [];
  let upgraded = 0;

  const fetchFn = ((input: string | URL, init?: RequestInit) => {
    const url = new URL(String(input));
    requests.push({ url, headers: new Headers(init?.headers) });
    if (db.fail === "network") return Promise.reject(new TypeError("network down"));
    if (db.fail === "http") return Promise.resolve(new Response("server error", { status: 500 }));
    const id = url.searchParams.get("id")?.replace(/^eq\./, "");
    const matching = id === undefined ? db.rows : db.rows.filter((r) => r.id === id);
    return Promise.resolve(new Response(JSON.stringify(matching), { status: 200 }));
  }) as typeof fetch;

  const connectGemini: GeminiConnector = (_token, setup, handlers) => {
    setups.push(setup);
    links.push(handlers);
    setTimeout(() => handlers.onEvent({ type: "setup_complete" }), 0);
    return Promise.resolve({ send: () => {}, close: () => {} });
  };

  const handler = createHandler({
    config: cfg,
    log: createLogger((line) => logs.push(line), () => "T"),
    connectGemini,
    fetchFn,
    now: () => 0,
    upgrade: (() => {
      upgraded++;
      const socket = fakeSocket();
      sockets.push(socket);
      return { socket: socket as unknown as WebSocket, response: new Response(null, { status: 200 }) };
    }) as unknown as typeof Deno.upgradeWebSocket,
  });
  const info = { remoteAddr: { hostname: "10.0.0.1", port: 1, transport: "tcp" } } as unknown as Deno.ServeHandlerInfo;

  const settle = async () => {
    for (let i = 0; i < 4; i++) await flush();
  };
  const connect = async (): Promise<number> => {
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
    if (res.status === 200) {
      const socket = sockets[sockets.length - 1];
      socket.onopen?.(new Event("open"));
      socket.onmessage?.({ data: HELLO } as MessageEvent);
      await settle();
    }
    return res.status;
  };
  const deviceText = async (text: string) => {
    sockets[sockets.length - 1].onmessage?.({ data: text } as MessageEvent);
    await settle();
  };
  return { connect, deviceText, logs, requests, setups, links, sockets, upgraded: () => upgraded };
}

function instructionOf(setup: string): string {
  return JSON.parse(setup).setup.systemInstruction.parts[0].text;
}

function assertRefusedBeforeGemini(g: ReturnType<typeof gateway>, status: number) {
  assertEquals(status, 503);
  assertEquals(g.upgraded(), 0);
  assertEquals(g.setups.length, 0);
}

function lastEvent(g: ReturnType<typeof gateway>): Record<string, unknown> {
  return JSON.parse(g.logs[g.logs.length - 1]);
}

// --- valid loading and prompt parity --------------------------------------------------

Deno.test("a configured activity is loaded by id with the publishable key and composed like Flutter", async () => {
  const g = gateway(await config(), { rows: [row()] });
  assertEquals(await g.connect(), 200);
  assertEquals(g.requests.length, 1);
  const request = g.requests[0];
  assertEquals(request.url.pathname, "/rest/v1/activities");
  assertEquals(request.url.searchParams.get("id"), `eq.${ID}`);
  assertEquals(request.headers.get("apikey"), PUBLISHABLE);
  assertEquals(request.headers.get("authorization"), `Bearer ${PUBLISHABLE}`);
  assertEquals(g.setups.length, 1);
  assertEquals(
    instructionOf(g.setups[0]),
    composeSystemInstruction({ prompt: PROMPT, participants: [CHILD, "Ana"] }),
  );
  assert(instructionOf(g.setups[0]).endsWith(`${CHILD}, Ana.`), "participants in order");
  assert(g.sockets[0].sent.some((m) => m.includes('"t":"ready"')), "device got ready");
});

Deno.test("an activity without participants composes without the participants sentence", async () => {
  const g = gateway(await config(), { rows: [row({ participants: [] })] });
  assertEquals(await g.connect(), 200);
  assertEquals(instructionOf(g.setups[0]), composeSystemInstruction({ prompt: PROMPT, participants: [] }));
});

// --- refusals: nothing is upgraded and Gemini is never opened -----------------------

Deno.test("a missing activity is refused before Gemini", async () => {
  const g = gateway(await config(), { rows: [] });
  assertRefusedBeforeGemini(g, await g.connect());
  const e = lastEvent(g);
  assertEquals(e.event, "activity_unavailable");
  assertEquals(e.code, "missing");
  assertEquals(e.activity, ID);
});

Deno.test("a malformed activity id is refused before any request", async () => {
  for (const bad of ["not-a-uuid", "0F8FAD5B-D9CB-469F-A165-70867728950E", `${ID}&select=prompt`]) {
    assertThrows(() =>
      parseRegistry(JSON.stringify({
        "core2-01": { token_sha256: "0".repeat(64), activity_id: bad, enabled: true },
      }))
    );
    let calls = 0;
    const fetchFn = (() => {
      calls++;
      return Promise.resolve(new Response("[]"));
    }) as unknown as typeof fetch;
    let threw = false;
    try {
      await fetchActivityById(fetchFn, "https://example.test", PUBLISHABLE, bad);
    } catch {
      threw = true;
    }
    assert(threw, "malformed id refused");
    assertEquals(calls, 0);
  }
  assertThrows(() => parseActivityRow(row({ id: "not-a-uuid" })));
});

Deno.test("a disabled activity is refused before Gemini", async () => {
  const g = gateway(await config(), { rows: [row({ enabled: false })] });
  assertRefusedBeforeGemini(g, await g.connect());
  assertEquals(lastEvent(g).code, "disabled");
});

Deno.test("an oversized prompt or response is refused before Gemini", async () => {
  const g = gateway(await config(), { rows: [row({ prompt: "a".repeat(8_001) })] });
  assertRefusedBeforeGemini(g, await g.connect());
  assertEquals(lastEvent(g).event, "activity_invalid");
  assertEquals(lastEvent(g).code, "prompt_too_long");

  const huge = (() =>
    Promise.resolve(new Response(" ".repeat(MAX_ACTIVITY_RESPONSE_CHARS + 1) + "[]"))) as unknown as typeof fetch;
  let threw = false;
  try {
    await fetchActivityById(huge, "https://example.test", PUBLISHABLE, ID);
  } catch {
    threw = true;
  }
  assert(threw, "response size bounded");
});

Deno.test("too many participants are refused before Gemini", async () => {
  const g = gateway(await config(), { rows: [row({ participants: Array(13).fill("Ana") })] });
  assertRefusedBeforeGemini(g, await g.connect());
  assertEquals(lastEvent(g).code, "too_many_participants");
});

Deno.test("a Supabase HTTP or network failure is refused before Gemini", async () => {
  for (const fail of ["http", "network"] as const) {
    const g = gateway(await config(), { rows: [row()], fail });
    assertRefusedBeforeGemini(g, await g.connect());
    assertEquals(lastEvent(g).event, "activities_unavailable");
  }
  const malformed = gateway(await config(), { rows: [row({ interaction_mode: "voice" })] });
  assertRefusedBeforeGemini(malformed, await malformed.connect());
});

// --- snapshot lifetime -------------------------------------------------------------------

Deno.test("the activity snapshot is immutable for the session and refreshed on reconnect", async () => {
  const db: Db = { rows: [row()] };
  const g = gateway(await config(), db);
  assertEquals(await g.connect(), 200);
  const first = instructionOf(g.setups[0]);

  // The activity is edited in the Dashboard while the session runs.
  db.rows = [row({ prompt: "Activitate nouă, schimbată." })];

  // Gemini closes and the next turn reopens it within the SAME device session:
  // same snapshot, no reload from Supabase.
  g.links[0].onClose(1000);
  await g.deviceText(`{"t":"turn_start","turn":1}`);
  assertEquals(g.setups.length, 2);
  assertEquals(instructionOf(g.setups[1]), first);
  assertEquals(g.requests.length, 1);

  // A reconnect loads a fresh snapshot.
  assertEquals(await g.connect(), 200);
  assertEquals(g.requests.length, 2);
  const refreshed = instructionOf(g.setups[g.setups.length - 1]);
  assert(refreshed !== first, "new snapshot");
  assert(refreshed.includes("Activitate nouă, schimbată."), "edited prompt");

  const parsed = parseActivityRow(row());
  assert(Object.isFrozen(parsed) && Object.isFrozen(parsed.participants), "frozen snapshot");
});

// --- redaction --------------------------------------------------------------------------

Deno.test("no prompt, title or child name reaches any log line, on success or failure", async () => {
  const all: string[] = [];
  const scenarios: Array<{ db: Db; activity: string | null }> = [
    { db: { rows: [row()] }, activity: ID },
    { db: { rows: [row()] }, activity: null },
    { db: { rows: [] }, activity: ID },
    { db: { rows: [row({ enabled: false })] }, activity: ID },
    { db: { rows: [row({ prompt: `${"a".repeat(8_001)}PROMPTSENTINEL` })] }, activity: ID },
    { db: { rows: [row({ participants: Array(13).fill(CHILD) })] }, activity: ID },
    { db: { rows: [row({ participants: [`${CHILD}1`] })] }, activity: ID },
    { db: { rows: [row()], fail: "http" }, activity: ID },
    { db: { rows: [row()], fail: "network" }, activity: ID },
  ];
  for (const s of scenarios) {
    const g = gateway(await config(s.activity), s.db);
    await g.connect();
    all.push(...g.logs);
  }
  assert(all.length > 0, "logs were produced");
  for (const line of all) {
    for (const sentinel of SENTINELS) {
      assert(!line.includes(sentinel), `log must not contain ${sentinel}`);
    }
  }
});
