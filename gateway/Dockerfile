# TTH gateway container for Google Cloud Run.
#
# Build only after `deno task generate` + `deno task test` pass: the base
# prompt is baked in from src/generated (the runtime never reads Dart).
#
# Cloud Run settings (PHASE6_PLAN D1):
#   --timeout=3600            WebSocket requests end at the request timeout;
#                             the device reconnects proactively at ~55 min.
#   --set-secrets             TTH_DEVICES, GATEWAY_MINT_SECRET from Secret Manager
#   --set-env-vars            SUPABASE_URL, SUPABASE_PUBLISHABLE_KEY, TRUST_PROXY=1
# Public deployment is GATED on: gateway-gemini-token deployed with its secret,
# AND supabase/pending/…restrict_activity_writes.sql applied.

FROM denoland/deno:2.9.6

WORKDIR /app
COPY deno.json main.ts ./
COPY src ./src
RUN deno cache main.ts

USER deno
ENV PORT=8080
EXPOSE 8080
CMD ["run", "--allow-net", "--allow-env", "main.ts"]
