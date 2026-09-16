// deno test supabase/functions/gateway-gemini-token/
//
// Missing, malformed, wrong, expired, future and replayed credentials; rate
// limiting; fail-closed admission; upstream failure; and the valid path. The
// signature vector is the SAME one the gateway's token client is tested
// against (gateway/tests/adapters_test.ts), computed independently with Node.

import { AdmitResult, expectedSignature, handle, HandlerDeps, MINT_HEADER } from "./handler.ts";
import { mintEphemeralToken } from "./mint.ts";

function assert(condition: unknown, message = "assertion failed"): asserts condition {
  if (!condition) throw new Error(message);
}
function assertEquals(actual: unknown, expected: unknown) {
  if (actual !== expected) throw new Error(`expected ${expected}, got ${actual}`);
}

const SECRET = "test-gateway-secret-that-is-long-enough-32+";
const VECTOR = {
  secret: "test-gateway-secret",
  ts: "1757600000000",
  nonce: "AAECAwQFBgcICQoLDA0ODw",
  sig: "tS3Bh2_7tIG4Kb1iOMUJc5l58sBNjJzCERiJS7TqVjM",
};
const NOW = 1757600000000;
const NONCE = "AAECAwQFBgcICQoLDA0ODw";

function deps(overrides: Partial<HandlerDeps> = {}) {
  const calls = { admit: 0, mint: 0, logs: [] as string[] };
  const d: HandlerDeps = {
    secret: SECRET,
    nowMs: () => NOW,
    admit: () => {
      calls.admit++;
      return Promise.resolve("ok" as AdmitResult);
    },
    mint: () => {
      calls.mint++;
      return Promise.resolve("ephemeral-token");
    },
    log: (event) => {
      calls.logs.push(event);
    },
    ...overrides,
  };
  return { d, calls };
}

async function request(header: string | null, method = "POST"): Promise<Request> {
  const headers = new Headers();
  if (header !== null) headers.set(MINT_HEADER, header);
  return new Request("https://x.test/functions/v1/gateway-gemini-token", { method, headers });
}

async function signed(ts = NOW, nonce = NONCE, secret = SECRET): Promise<string> {
  return `ts=${ts},nonce=${nonce},sig=${await expectedSignature(secret, String(ts), nonce)}`;
}

Deno.test("the signature matches the shared cross-language vector", async () => {
  assertEquals(await expectedSignature(VECTOR.secret, VECTOR.ts, VECTOR.nonce), VECTOR.sig);
});

Deno.test("valid credential: one token, nonce admitted once", async () => {
  const { d, calls } = deps();
  const res = await handle(await request(await signed()), d);
  assertEquals(res.status, 200);
  assertEquals((await res.json()).token, "ephemeral-token");
  assertEquals(calls.admit, 1);
  assertEquals(calls.mint, 1);
});

Deno.test("missing credential: 401, nothing touched", async () => {
  const { d, calls } = deps();
  const res = await handle(await request(null), d);
  assertEquals(res.status, 401);
  assertEquals(calls.admit, 0);
  assertEquals(calls.mint, 0);
});

Deno.test("malformed credentials: 401", async () => {
  const { d } = deps();
  for (
    const header of [
      "Bearer something",
      `ts=${NOW},nonce=short,sig=x`,
      `ts=123,nonce=${NONCE},sig=${"a".repeat(43)}`,
      `nonce=${NONCE},ts=${NOW},sig=${"a".repeat(43)}`,
    ]
  ) {
    assertEquals((await handle(await request(header), d)).status, 401);
  }
});

Deno.test("wrong secret: 401, and the database is never reached", async () => {
  const { d, calls } = deps();
  const res = await handle(await request(await signed(NOW, NONCE, "a-different-secret-of-sufficient-length")), d);
  assertEquals(res.status, 401);
  assertEquals(calls.admit, 0);
});

Deno.test("tampered nonce or timestamp: 401", async () => {
  const { d } = deps();
  const good = await signed();
  const otherNonce = good.replace(NONCE, "BBECAwQFBgcICQoLDA0ODw");
  assertEquals((await handle(await request(otherNonce), d)).status, 401);
  const otherTs = good.replace(`ts=${NOW}`, `ts=${NOW + 1}`);
  assertEquals((await handle(await request(otherTs), d)).status, 401);
});

Deno.test("expired credential (older than 60 s): 401", async () => {
  const { d, calls } = deps();
  const res = await handle(await request(await signed(NOW - 60_001)), d);
  assertEquals(res.status, 401);
  assertEquals(calls.admit, 0);
});

Deno.test("future credential (more than 60 s ahead): 401", async () => {
  const { d } = deps();
  assertEquals((await handle(await request(await signed(NOW + 60_001)), d)).status, 401);
});

Deno.test("credential at the edge of the window is accepted", async () => {
  const { d } = deps();
  assertEquals((await handle(await request(await signed(NOW - 60_000)), d)).status, 200);
});

Deno.test("replayed nonce: 401 with no token", async () => {
  const { d, calls } = deps({ admit: () => Promise.resolve("replay") });
  const res = await handle(await request(await signed()), d);
  assertEquals(res.status, 401);
  assertEquals(calls.mint, 0);
});

Deno.test("rate limited: 429 with Retry-After", async () => {
  const { d, calls } = deps({ admit: () => Promise.resolve("rate_limited") });
  const res = await handle(await request(await signed()), d);
  assertEquals(res.status, 429);
  assertEquals(res.headers.get("Retry-After"), "60");
  assertEquals(calls.mint, 0);
});

Deno.test("replay store unavailable: fail closed with 503", async () => {
  const { d, calls } = deps({ admit: () => Promise.reject(new Error("db down")) });
  const res = await handle(await request(await signed()), d);
  assertEquals(res.status, 503);
  assertEquals(calls.mint, 0);
});

Deno.test("Gemini failure: 502, no token", async () => {
  const { d } = deps({ mint: () => Promise.reject(new Error("upstream")) });
  assertEquals((await handle(await request(await signed()), d)).status, 502);
});

Deno.test("unconfigured or weak secret: 500 before any other work", async () => {
  for (const secret of [undefined, "", "too-short"]) {
    const { d, calls } = deps({ secret });
    assertEquals((await handle(await request(await signed()), d)).status, 500);
    assertEquals(calls.admit, 0);
  }
});

Deno.test("only POST is accepted", async () => {
  const { d } = deps();
  assertEquals((await handle(await request(await signed(), "GET"), d)).status, 405);
});

Deno.test("all authentication failures look identical", async () => {
  const { d } = deps();
  const bodies = new Set<string>();
  for (const header of [null, "junk", await signed(NOW - 120_000)]) {
    bodies.add(await (await handle(await request(header), d)).text());
  }
  assertEquals(bodies.size, 1);
});

Deno.test("logs never contain the header, nonce, signature or token", async () => {
  const lines: string[] = [];
  const { d } = deps({ log: (e, f) => lines.push(JSON.stringify({ e, f })) });
  await handle(await request(await signed()), d);
  await handle(await request("junk"), d);
  const all = lines.join("\n");
  assert(!all.includes(NONCE) && !all.includes("ephemeral-token") && !all.includes(SECRET));
});

Deno.test("mint uses the same Gemini constraints as gemini-token", async () => {
  let body: Record<string, unknown> = {};
  let key = "";
  const fetchFn = ((_url: string, init: RequestInit) => {
    body = JSON.parse(init.body as string);
    key = new Headers(init.headers).get("x-goog-api-key") ?? "";
    return Promise.resolve(new Response('{"name":"auth_tokens/abc"}', { status: 200 }));
  }) as unknown as typeof fetch;
  const token = await mintEphemeralToken("k", NOW, fetchFn);
  assertEquals(token, "auth_tokens/abc");
  assertEquals(key, "k");
  assertEquals(body.uses, 1);
  assertEquals(
    JSON.stringify(body.liveConnectConstraints),
    '{"model":"models/gemini-3.1-flash-live-preview","config":{"responseModalities":["AUDIO"]}}',
  );
  assertEquals(body.newSessionExpireTime, new Date(NOW + 60_000).toISOString());
  assertEquals(body.expireTime, new Date(NOW + 30 * 60_000).toISOString());
});
