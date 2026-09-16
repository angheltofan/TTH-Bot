// deno test supabase/functions/gateway-gemini-token/
//
// Missing, malformed, wrong, expired, future and replayed credentials; rate
// limiting; fail-closed admission; upstream failure; and the valid path. The
// signature vector is the SAME one the gateway's token client is tested
// against (gateway/tests/adapters_test.ts), computed independently with Node.

import { AdmitResult, expectedSignature, handle, HandlerDeps, MINT_HEADER } from "./handler.ts";
import { GEMINI_AUTH_TOKENS_URL, mintEphemeralToken, rfc3339Seconds, UpstreamMintError } from "./mint.ts";

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

Deno.test("upstream failure: the numeric status is logged, the client body stays generic", async () => {
  const lines: string[] = [];
  const { d } = deps({
    mint: () => Promise.reject(new UpstreamMintError(400)),
    log: (e, f) => lines.push(JSON.stringify(f === undefined ? { event: e } : { event: e, ...f })),
  });
  const res = await handle(await request(await signed()), d);
  assertEquals(res.status, 502);
  const body = await res.text();
  assertEquals(body, '{"error":"Failed to mint Gemini ephemeral token"}');
  assert(!body.includes("400"), "no upstream status in the client body");
  assertEquals(lines[lines.length - 1], '{"event":"gateway_mint_upstream_failed","status":400}');
});

Deno.test("upstream failure log carries nothing but the status", async () => {
  const API_KEY = "API-KEY-SENTINEL-0123456789";
  const UPSTREAM_MESSAGE = "API key not valid. Please pass a valid API key. BODY-SENTINEL";
  const fetchFn = (() =>
    Promise.resolve(
      new Response(JSON.stringify({ error: { message: UPSTREAM_MESSAGE } }), {
        status: 403,
        headers: { "x-upstream-header": "HEADER-SENTINEL" },
      }),
    )) as unknown as typeof fetch;

  let thrown: unknown;
  try {
    await mintEphemeralToken(API_KEY, NOW, fetchFn);
  } catch (e) {
    thrown = e;
  }
  assert(thrown instanceof UpstreamMintError, "a typed upstream error");
  assertEquals((thrown as UpstreamMintError).status, 403);
  const errorText = `${(thrown as Error).message} ${(thrown as Error).stack ?? ""}`;
  assert(!errorText.includes("BODY-SENTINEL") && !errorText.includes(API_KEY), "the error holds no body or key");

  const lines: string[] = [];
  const header = await signed();
  const { d } = deps({
    mint: () => mintEphemeralToken(API_KEY, NOW, fetchFn),
    log: (e, f) => lines.push(JSON.stringify(f === undefined ? { event: e } : { event: e, ...f })),
  });
  const res = await handle(await request(header), d);
  assertEquals(res.status, 502);
  const all = lines.join("\n");
  assertEquals(lines[lines.length - 1], '{"event":"gateway_mint_upstream_failed","status":403}');
  for (
    const forbidden of [
      API_KEY,
      "BODY-SENTINEL",
      "API key not valid",
      "HEADER-SENTINEL",
      NONCE,
      header,
      header.split("sig=")[1],
      SECRET,
      "ephemeral-token",
    ]
  ) {
    assert(!all.includes(forbidden), `log must not contain ${forbidden.slice(0, 12)}…`);
  }
});

Deno.test("a failure without a valid HTTP status logs the event alone", async () => {
  for (
    const error of [
      new Error("unexpected auth_tokens response"),
      { status: "400" },
      { status: 42 },
      { status: 400.5 },
      null,
    ]
  ) {
    const lines: string[] = [];
    const { d } = deps({
      mint: () => Promise.reject(error),
      log: (e, f) => lines.push(JSON.stringify(f === undefined ? { event: e } : { event: e, ...f })),
    });
    assertEquals((await handle(await request(await signed()), d)).status, 502);
    assertEquals(lines[lines.length - 1], '{"event":"gateway_mint_upstream_failed"}');
  }
});

// The exact outgoing REST request (docs: live-api/ephemeral-tokens).
async function captureMintRequest(nowMs: number) {
  const seen = { url: "", method: "", headers: new Headers(), bodyText: "", calls: 0 };
  const fetchFn = ((url: string, init: RequestInit) => {
    seen.calls++;
    seen.url = url;
    seen.method = init.method ?? "";
    seen.headers = new Headers(init.headers);
    seen.bodyText = init.body as string;
    return Promise.resolve(new Response('{"name":"auth_tokens/TOKEN-SENTINEL"}', { status: 200 }));
  }) as unknown as typeof fetch;
  const token = await mintEphemeralToken("API-KEY-SENTINEL", nowMs, fetchFn);
  return { seen, token };
}

Deno.test("REST request: endpoint, method and the API key only in x-goog-api-key", async () => {
  const { seen, token } = await captureMintRequest(NOW);
  assertEquals(seen.calls, 1);
  assertEquals(seen.url, "https://generativelanguage.googleapis.com/v1beta/auth_tokens");
  assertEquals(GEMINI_AUTH_TOKENS_URL, seen.url);
  assertEquals(seen.method, "POST");
  assertEquals(seen.headers.get("x-goog-api-key"), "API-KEY-SENTINEL");
  assertEquals(seen.headers.get("content-type"), "application/json");
  assertEquals(seen.headers.get("authorization"), null);
  assert(!seen.url.includes("API-KEY-SENTINEL"), "no key in the URL");
  assert(!seen.bodyText.includes("API-KEY-SENTINEL"), "no key in the body");
  assertEquals(token, "auth_tokens/TOKEN-SENTINEL");
});

Deno.test("REST request: exactly uses, expireTime and newSessionExpireTime", async () => {
  const { seen } = await captureMintRequest(NOW);
  const body = JSON.parse(seen.bodyText) as Record<string, unknown>;
  assertEquals(Object.keys(body).sort().join(","), "expireTime,newSessionExpireTime,uses");
  assertEquals(body.uses, 1);
  // No SDK-style constraint wrapper, no snake_case, no model constraint.
  for (const forbidden of ["liveConnectConstraints", "config", "model", "expire_time", "new_session_expire_time", "live_connect_constraints", "responseModalities", "gemini-"]) {
    assert(!seen.bodyText.includes(forbidden), `body must not contain ${forbidden}`);
  }
});

Deno.test("REST request: whole-second UTC RFC 3339 timestamps, 60 s and 30 min ahead", async () => {
  const now = Date.UTC(2026, 8, 16, 11, 10, 5, 987); // with milliseconds on purpose
  const { seen } = await captureMintRequest(now);
  const body = JSON.parse(seen.bodyText) as { expireTime: string; newSessionExpireTime: string };
  const RFC3339_UTC_SECONDS = /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$/;
  assert(RFC3339_UTC_SECONDS.test(body.expireTime), "expireTime format");
  assert(RFC3339_UTC_SECONDS.test(body.newSessionExpireTime), "newSessionExpireTime format");
  assertEquals(body.newSessionExpireTime, "2026-09-16T11:11:05Z");
  assertEquals(body.expireTime, "2026-09-16T11:40:05Z");
  assertEquals(rfc3339Seconds(Date.UTC(2026, 0, 1, 0, 0, 0, 999)), "2026-01-01T00:00:00Z");
});

Deno.test("successful mint: the handler logs neither the token nor the API key", async () => {
  const lines: string[] = [];
  const header = await signed();
  const { d } = deps({
    mint: async () => (await captureMintRequest(NOW)).token,
    log: (e, f) => lines.push(JSON.stringify(f === undefined ? { event: e } : { event: e, ...f })),
  });
  const res = await handle(await request(header), d);
  assertEquals(res.status, 200);
  const all = lines.join("\n");
  assertEquals(all, '{"event":"gateway_mint_ok"}');
  for (const forbidden of ["TOKEN-SENTINEL", "API-KEY-SENTINEL", NONCE, header, SECRET]) {
    assert(!all.includes(forbidden), "no credential or token in the log");
  }
});
