// Request handling for `gateway-gemini-token` (PHASE6_PLAN §4.3, D2, D10).
//
// Mints ONE single-use Gemini Live ephemeral token for the TTH gateway —
// and only for the gateway. The caller proves it holds GATEWAY_MINT_SECRET
// with an HMAC-signed, short-lived assertion; the secret itself never
// travels:
//
//   x-tth-gateway-auth: ts=<unix ms>,nonce=<16 bytes base64url>,sig=<base64url>
//   sig = HMAC-SHA256(secret, "tth-gateway-mint\n" + ts + "\n" + nonce)
//
// Order of checks — cheap and database-free first, so an unauthenticated
// caller can never write to the database:
//   1. POST only;
//   2. the secret is configured;
//   3. the header is well formed;
//   4. the timestamp is within ±60 s (expired or future → refused);
//   5. the signature matches (constant-time);
//   6. the nonce is new and the mint rate is within limit (Postgres);
//   7. mint.
// Every authentication failure gets the same 401 body, so it reveals
// nothing about which check failed.
//
// The Flutter app's existing `gemini-token` function is untouched; the
// gateway never uses it.

export const MINT_HEADER = "x-tth-gateway-auth";
export const MINT_CONTEXT = "tth-gateway-mint";
export const MAX_SKEW_MS = 60_000;
const HEADER_RE = /^ts=(\d{13}),nonce=([A-Za-z0-9_-]{22}),sig=([A-Za-z0-9_-]{43})$/;
const MIN_SECRET_LENGTH = 32;

export type AdmitResult = "ok" | "replay" | "rate_limited";

export interface HandlerDeps {
  secret: string | undefined;
  nowMs(): number;
  // Records the nonce and applies the rate limit, atomically.
  admit(nonce: string): Promise<AdmitResult>;
  mint(): Promise<string>;
  log(event: string, fields?: Record<string, string | number>): void;
}

function json(body: unknown, status: number, extra: HeadersInit = {}): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { "Content-Type": "application/json", ...extra },
  });
}

const UNAUTHORIZED = () => json({ error: "unauthorized" }, 401);

function base64Url(bytes: Uint8Array): string {
  let binary = "";
  for (const b of bytes) binary += String.fromCharCode(b);
  return btoa(binary).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}

export async function expectedSignature(
  secret: string,
  ts: string,
  nonce: string,
): Promise<string> {
  const key = await crypto.subtle.importKey(
    "raw",
    new TextEncoder().encode(secret),
    { name: "HMAC", hash: "SHA-256" },
    false,
    ["sign"],
  );
  const mac = await crypto.subtle.sign(
    "HMAC",
    key,
    new TextEncoder().encode(`${MINT_CONTEXT}\n${ts}\n${nonce}`),
  );
  return base64Url(new Uint8Array(mac));
}

export function timingSafeEqualStrings(a: string, b: string): boolean {
  const ea = new TextEncoder().encode(a);
  const eb = new TextEncoder().encode(b);
  if (ea.length !== eb.length) return false;
  let diff = 0;
  for (let i = 0; i < ea.length; i++) diff |= ea[i] ^ eb[i];
  return diff === 0;
}

export async function handle(req: Request, deps: HandlerDeps): Promise<Response> {
  if (req.method !== "POST") return json({ error: "method not allowed" }, 405);

  const secret = deps.secret;
  if (!secret || secret.length < MIN_SECRET_LENGTH) {
    deps.log("gateway_mint_misconfigured");
    return json({ error: "not configured" }, 500);
  }

  const header = req.headers.get(MINT_HEADER);
  const match = header ? HEADER_RE.exec(header) : null;
  if (!match) {
    deps.log("gateway_mint_denied", { reason: "malformed" });
    return UNAUTHORIZED();
  }
  const [, ts, nonce, sig] = match;

  const skew = Math.abs(deps.nowMs() - Number(ts));
  if (!Number.isFinite(skew) || skew > MAX_SKEW_MS) {
    deps.log("gateway_mint_denied", { reason: "stale" });
    return UNAUTHORIZED();
  }

  const expected = await expectedSignature(secret, ts, nonce);
  if (!timingSafeEqualStrings(sig, expected)) {
    deps.log("gateway_mint_denied", { reason: "signature" });
    return UNAUTHORIZED();
  }

  let admission: AdmitResult;
  try {
    admission = await deps.admit(nonce);
  } catch {
    deps.log("gateway_mint_admit_failed");
    // Fail closed: without the replay store there is no replay protection.
    return json({ error: "unavailable" }, 503);
  }
  if (admission === "replay") {
    deps.log("gateway_mint_denied", { reason: "replay" });
    return UNAUTHORIZED();
  }
  if (admission === "rate_limited") {
    deps.log("gateway_mint_rate_limited");
    return json({ error: "rate limited" }, 429, { "Retry-After": "60" });
  }

  try {
    const token = await deps.mint();
    deps.log("gateway_mint_ok");
    return json({ token }, 200);
  } catch {
    deps.log("gateway_mint_upstream_failed");
    return json({ error: "Failed to mint Gemini ephemeral token" }, 502);
  }
}
