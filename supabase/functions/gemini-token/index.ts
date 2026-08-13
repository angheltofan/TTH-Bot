// Supabase Edge Function: gemini-token
//
// Mints one short-lived Gemini Live ephemeral token per request so the
// Flutter app (Android + web) never holds the permanent Gemini API key.
// The permanent key lives only as this function's GEMINI_API_KEY secret:
//
//   supabase secrets set GEMINI_API_KEY=your-real-key --project-ref gashjedpdvcrzwryjiva
//
// Source of truth: https://ai.google.dev/gemini-api/docs/live-api/ephemeral-tokens
//
// AUTH NOTE: this function runs with verify_jwt = false (see
// supabase/config.toml) because the Flutter app calls it with a Supabase
// *publishable* key, which is not a JWT —
// https://supabase.com/docs/guides/functions/auth. EduBot V1 has no
// end-user accounts, so there is currently no caller identity to check
// here beyond "has the publishable key" — the same trust model already
// used for the activities table's RLS policies (see the migration).
// Before this function is reachable from outside a controlled pilot, add a
// real authorization check here (e.g. Supabase Auth + a verified user, or
// at minimum a shared secret / rate limiting) so it can't be used to mint
// unlimited tokens against your Gemini quota.

const GEMINI_MODEL = "models/gemini-3.1-flash-live-preview";
const GEMINI_AUTH_TOKENS_URL =
  "https://generativelanguage.googleapis.com/v1beta/auth_tokens";

Deno.serve(async (_req: Request) => {
  const apiKey = Deno.env.get("GEMINI_API_KEY");
  if (!apiKey) {
    console.error("gemini-token: GEMINI_API_KEY secret is not set");
    return jsonResponse({ error: "GEMINI_API_KEY is not configured" }, 500);
  }

  const now = Date.now();
  // Per the ephemeral-tokens doc: 1 minute to start a Live session with
  // this token (newSessionExpireTime), 30 minutes for that session to stay
  // open once started (expireTime).
  const newSessionExpireTime = new Date(now + 60 * 1000).toISOString();
  const expireTime = new Date(now + 30 * 60 * 1000).toISOString();

  let geminiResponse: Response;
  try {
    geminiResponse = await fetch(GEMINI_AUTH_TOKENS_URL, {
      method: "POST",
      headers: {
        "x-goog-api-key": apiKey,
        "Content-Type": "application/json",
      },
      body: JSON.stringify({
        uses: 1,
        expireTime,
        newSessionExpireTime,
        // Locks the token to the model/response-modality EduBot actually
        // uses, so a leaked token can't be used for anything else.
        liveConnectConstraints: {
          model: GEMINI_MODEL,
          config: {
            responseModalities: ["AUDIO"],
          },
        },
      }),
    });
  } catch (error) {
    console.error("gemini-token: request to Gemini failed", error);
    return jsonResponse({ error: "Could not reach Gemini" }, 502);
  }

  if (!geminiResponse.ok) {
    const errorBody = await geminiResponse.text();
    console.error(
      "gemini-token: auth_tokens request failed",
      geminiResponse.status,
      errorBody,
    );
    return jsonResponse(
      { error: "Failed to mint Gemini ephemeral token" },
      502,
    );
  }

  const data = await geminiResponse.json();
  // The Gemini SDKs expose this same value as `token.name`; the REST
  // response returns it as the AuthToken resource's `name` field.
  const token = data?.name;
  if (typeof token !== "string" || token.length === 0) {
    console.error("gemini-token: unexpected auth_tokens response shape", data);
    return jsonResponse({ error: "Unexpected Gemini response shape" }, 502);
  }

  return jsonResponse({ token });
});

function jsonResponse(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { "Content-Type": "application/json" },
  });
}
