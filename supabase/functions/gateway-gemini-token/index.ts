// Supabase Edge Function: gateway-gemini-token
//
// The TTH gateway's ONLY way to obtain a Gemini ephemeral token. See
// handler.ts for the authentication scheme. Secrets (never in Git):
//
//   supabase secrets set GATEWAY_MINT_SECRET=<at least 32 random chars> --project-ref <ref>
//   (GEMINI_API_KEY is already set for gemini-token)
//
// The same GATEWAY_MINT_SECRET goes into Cloud Run Secret Manager for the
// gateway. SUPABASE_URL and SUPABASE_SERVICE_ROLE_KEY are provided by the
// Edge runtime and are used only to call gateway_mint_admit (nonce replay
// store + rate limit; migration 20260911190000_gateway_mint_admit.sql).
//
// verify_jwt = false (supabase/config.toml): this function authenticates the
// caller itself; a JWT would require giving the gateway the service-role key.

import { AdmitResult, handle } from "./handler.ts";
import { mintEphemeralToken } from "./mint.ts";

const RATE_PER_MINUTE = Number(Deno.env.get("GATEWAY_MINT_RATE_PER_MIN") ?? "30");

async function admit(nonce: string): Promise<AdmitResult> {
  const url = Deno.env.get("SUPABASE_URL");
  const serviceKey = Deno.env.get("SUPABASE_SERVICE_ROLE_KEY");
  if (!url || !serviceKey) throw new Error("service configuration missing");
  const response = await fetch(`${url}/rest/v1/rpc/gateway_mint_admit`, {
    method: "POST",
    headers: {
      apikey: serviceKey,
      Authorization: `Bearer ${serviceKey}`,
      "Content-Type": "application/json",
    },
    body: JSON.stringify({
      p_nonce: nonce,
      p_limit: RATE_PER_MINUTE,
      p_window_seconds: 60,
    }),
  });
  if (!response.ok) throw new Error(`admit failed: ${response.status}`);
  const result = await response.json();
  if (result !== "ok" && result !== "replay" && result !== "rate_limited") {
    throw new Error("unexpected admit result");
  }
  return result;
}

Deno.serve((req) =>
  handle(req, {
    secret: Deno.env.get("GATEWAY_MINT_SECRET"),
    nowMs: () => Date.now(),
    admit,
    mint: () => {
      const apiKey = Deno.env.get("GEMINI_API_KEY");
      if (!apiKey) return Promise.reject(new Error("GEMINI_API_KEY missing"));
      return mintEphemeralToken(apiKey, Date.now());
    },
    // Event names and fixed reasons only: never headers, nonces or tokens.
    log: (event, fields) => console.log(JSON.stringify({ event, ...fields })),
  })
);
