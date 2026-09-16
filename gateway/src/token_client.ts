// Gateway -> `gateway-gemini-token` Edge Function (PHASE6_PLAN §4.3, D2, D10).
//
// The gateway proves itself with an HMAC-signed, short-lived assertion, so
// the shared secret itself never travels:
//
//   x-tth-gateway-auth: ts=<unix ms>,nonce=<16 bytes base64url>,sig=<base64url>
//   sig = HMAC-SHA256(secret, "tth-gateway-mint\n" + ts + "\n" + nonce)
//
// The function rejects a missing/malformed header, a wrong signature, a
// timestamp more than 60 s off, and a replayed nonce. The secret lives in
// Cloud Run Secret Manager (GATEWAY_MINT_SECRET) — never in Git or firmware.
//
// A custom header (not Authorization) keeps the Supabase API gateway from
// interpreting it.

export const MINT_HEADER = "x-tth-gateway-auth";
export const MINT_CONTEXT = "tth-gateway-mint";

function base64Url(bytes: Uint8Array): string {
  let binary = "";
  for (const b of bytes) binary += String.fromCharCode(b);
  return btoa(binary).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}

export async function signMintAssertion(
  secret: string,
  tsMs: number,
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
    new TextEncoder().encode(`${MINT_CONTEXT}\n${tsMs}\n${nonce}`),
  );
  return base64Url(new Uint8Array(mac));
}

export async function buildMintHeader(
  secret: string,
  nowMs: number,
  nonceBytes: Uint8Array = crypto.getRandomValues(new Uint8Array(16)),
): Promise<string> {
  const nonce = base64Url(nonceBytes);
  const sig = await signMintAssertion(secret, nowMs, nonce);
  return `ts=${nowMs},nonce=${nonce},sig=${sig}`;
}

export interface MintOptions {
  fetchFn: typeof fetch;
  functionUrl: string; // https://<ref>.supabase.co/functions/v1/gateway-gemini-token
  publishableKey: string;
  secret: string;
  nowMs: number;
}

export async function mintGeminiToken(options: MintOptions): Promise<string> {
  const header = await buildMintHeader(options.secret, options.nowMs);
  const response = await options.fetchFn(options.functionUrl, {
    method: "POST",
    headers: {
      apikey: options.publishableKey,
      [MINT_HEADER]: header,
      "content-type": "application/json",
    },
    body: "{}",
  });
  if (!response.ok) throw new Error(`token_mint_failed_${response.status}`);
  const body: unknown = await response.json();
  const token = (body as { token?: unknown } | null)?.token;
  if (typeof token !== "string" || token.length === 0) {
    throw new Error("token_mint_bad_response");
  }
  return token;
}
