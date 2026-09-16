// Mints one Gemini Live ephemeral token over REST
// (https://ai.google.dev/gemini-api/docs/live-api/ephemeral-tokens):
//
//   POST https://generativelanguage.googleapis.com/v1beta/auth_tokens
//   x-goog-api-key: <GEMINI_API_KEY>
//   {"uses":1,"expireTime":"YYYY-MM-DDTHH:MM:SSZ","newSessionExpireTime":"YYYY-MM-DDTHH:MM:SSZ"}
//
// The SMALLEST valid request, deliberately without liveConnectConstraints:
// `liveConnectConstraints: { model, config }` is the SDK shape and is not
// part of the documented REST body (sending it made Google answer 400). The
// gateway already fixes the model and the session configuration itself, and
// the token is single use, short-lived, returned only after the gateway's
// HMAC authentication, and never leaves the gateway process.
//
// Lifetimes: a new session must start within 60 s; the token expires after
// 30 min (the documented defaults; the gateway retires a Gemini session at
// 28 min). Timestamps are whole-second UTC RFC 3339, as in the documentation.

// Google refused the auth_tokens request. Carries ONLY the numeric HTTP status
// (for diagnosis in the function log): never the response body, headers or
// anything from the request.
export class UpstreamMintError extends Error {
  readonly status: number;
  constructor(status: number) {
    super("auth_tokens request refused");
    this.name = "UpstreamMintError";
    this.status = status;
  }
}

export const GEMINI_AUTH_TOKENS_URL =
  "https://generativelanguage.googleapis.com/v1beta/auth_tokens";
export const NEW_SESSION_WINDOW_MS = 60 * 1000;
export const TOKEN_LIFETIME_MS = 30 * 60 * 1000;

// "2026-09-16T11:10:05Z": UTC, whole seconds, no fraction.
export function rfc3339Seconds(ms: number): string {
  return new Date(Math.floor(ms / 1000) * 1000).toISOString().replace(/\.\d{3}Z$/, "Z");
}

export function authTokenRequestBody(nowMs: number): string {
  return JSON.stringify({
    uses: 1,
    expireTime: rfc3339Seconds(nowMs + TOKEN_LIFETIME_MS),
    newSessionExpireTime: rfc3339Seconds(nowMs + NEW_SESSION_WINDOW_MS),
  });
}

export async function mintEphemeralToken(
  apiKey: string,
  nowMs: number,
  fetchFn: typeof fetch = fetch,
): Promise<string> {
  const response = await fetchFn(GEMINI_AUTH_TOKENS_URL, {
    method: "POST",
    headers: { "x-goog-api-key": apiKey, "Content-Type": "application/json" },
    body: authTokenRequestBody(nowMs),
  });
  if (!response.ok) {
    // The body is deliberately not read: it may echo request details.
    await response.body?.cancel();
    throw new UpstreamMintError(response.status);
  }
  const data = await response.json();
  const token = data?.name;
  if (typeof token !== "string" || token.length === 0) {
    throw new Error("unexpected auth_tokens response");
  }
  return token;
}
