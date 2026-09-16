// Mints one Gemini Live ephemeral token with exactly the constraints the
// existing `gemini-token` function uses (supabase/functions/gemini-token):
// single use, 60 s to start the session, 30 min lifetime, locked to the
// EduBot Live model and AUDIO responses. Kept as its own copy so the Flutter
// app's function stays untouched.

const GEMINI_MODEL = "models/gemini-3.1-flash-live-preview";
const GEMINI_AUTH_TOKENS_URL =
  "https://generativelanguage.googleapis.com/v1beta/auth_tokens";

export async function mintEphemeralToken(
  apiKey: string,
  nowMs: number,
  fetchFn: typeof fetch = fetch,
): Promise<string> {
  const response = await fetchFn(GEMINI_AUTH_TOKENS_URL, {
    method: "POST",
    headers: { "x-goog-api-key": apiKey, "Content-Type": "application/json" },
    body: JSON.stringify({
      uses: 1,
      expireTime: new Date(nowMs + 30 * 60 * 1000).toISOString(),
      newSessionExpireTime: new Date(nowMs + 60 * 1000).toISOString(),
      liveConnectConstraints: {
        model: GEMINI_MODEL,
        config: { responseModalities: ["AUDIO"] },
      },
    }),
  });
  if (!response.ok) throw new Error(`auth_tokens failed: ${response.status}`);
  const data = await response.json();
  const token = data?.name;
  if (typeof token !== "string" || token.length === 0) {
    throw new Error("unexpected auth_tokens response");
  }
  return token;
}
