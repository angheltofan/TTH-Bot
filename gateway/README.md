# TTH gateway

The security boundary between the M5Stack Core2 robots and Gemini Live
(design: [firmware/core2/docs/PHASE6_PLAN.md](../firmware/core2/docs/PHASE6_PLAN.md)).

```
Core2 ──wss tth.v1 (raw PCM, small JSON)──▶ gateway ──wss (JSON+base64)──▶ Gemini Live
                                              │
                                              ├── activities (Supabase, publishable key, read)
                                              └── gateway-gemini-token (HMAC-authenticated)
```

- The robot holds **no** Gemini key, token, Supabase secret or prompt.
- The permanent Gemini key stays in the Supabase function secrets.
- The gateway holds no Gemini key: it mints single-use ephemeral tokens
  through `gateway-gemini-token`, proving itself with an HMAC assertion
  (`GATEWAY_MINT_SECRET` is never transmitted).

## Layout

| Path | What |
|---|---|
| `src/protocol.ts` | the `tth.v1` codec (same vectors as the firmware's C++ tests) |
| `src/turns.ts`, `src/credit.ts`, `src/downstream.ts` | uint32 turn ids, downstream credit, the ordered/priority lanes |
| `src/session.ts` | one device connection ↔ one Gemini session: ordering, credit, cancel fence, idle/age/goAway |
| `src/server.ts` | pre-upgrade auth, rate limits, activity validation |
| `src/gemini.ts` | the Gemini adapter; setup identical to the Flutter push-to-talk setup |
| `src/prompt.ts`, `src/generated/` | prompt composition; the base prompt is generated **at build time** from the Dart source |
| `src/registry.ts`, `src/rate_limit.ts`, `src/limits.ts`, `src/log.ts` | device registry, throttling, explicit limits, allow-list logging |
| `manual/barge_in_probe.ts` | real-Gemini barge-in, **diagnostic**: first turn via `clientContent` (text) |
| `manual/barge_in_live_probe.ts` | real-Gemini barge-in, **production sequence**: manual `activityStart` · 16 kHz audio · `activityEnd` for both turns |

## Development

Deno 2.9+ (`D:\deno\deno.exe` on the dev machine). With Avast TLS interception,
prefix network commands with `DENO_TLS_CA_STORE=system`.

```bash
deno task generate        # re-extract the base prompt from lib/core/prompts/*.dart
deno task test            # all gateway tests (incl. prompt parity with Dart)
deno task check           # type-check the entry point
deno test supabase/functions/gateway-gemini-token/   # from the repo root
```

LAN gateway: copy `.env.example` to `.env` (ignored by Git), set `TLS_CERT_FILE`
and `TLS_KEY_FILE` to a dev certificate whose CA the device pins, then:

```bash
deno run --allow-net --allow-env --allow-read --env-file=.env main.ts
```

### LAN device testing without Gemini (Steps 6.2, 6.3)

The device's session gates (TLS, hello/ready, keepalive, auth rejection,
reconnects) need a gateway that reaches `ready`. `GEMINI_MODE=fake` completes
Gemini's setup locally (`src/fake_gemini.ts`): no Gemini, no token mint, no
mint secret, and — with `SUPABASE_URL` empty — a built-in development
activity. It is **refused** on Cloud Run (`K_SERVICE`) and with
`TRUST_PROXY=1`, and every `FAKE_*` setting is refused without it.

Since Step 6.3 its "model speech" is an **echo** of the device's user turn:
the realtime audio between `activityStart` and `activityEnd`, converted from
16 kHz to 24 kHz mono s16le by `src/resample.ts` (deterministic linear
interpolation, `floor(n·3/2)` output samples, bounded by the per-turn limits;
audio is never logged). Test controls for the device's gates P5, P7, P14:

| Setting | Range (default) | Effect |
|---|---|---|
| `FAKE_RESPONSE_DELAY_MS` | 0–60000 (300) | wait after the turn ends |
| `FAKE_FIRST_RESPONSE_DELAY_MS` | 0–60000 (unset) | replaces the delay for the first turn of the process only |
| `FAKE_ECHO_REPEAT` | 1–20 (1) | echo sent n times (within `maxModelAudioBytesPerTurn`) |
| `FAKE_CREDIT_HOLD_MS` | 0–60000 (0) | once the device's credit is exhausted, returned credit is held this long |
| `FAKE_VIOLATE_CREDIT` | 0–1 (0) | one model frame sent beyond the device's credit, once per process |

A new `activityStart` while a response is still scheduled cancels it with
`interrupted` + `turnComplete`, as Gemini does. The session also purges a
completed response that is still queued behind credit when the device
cancels it.

The exact, copy-pasteable Windows procedure (IP address, certificates,
provisioning on the robot's COM port, firewall, gates P3/P13/M2) is in
`firmware/core2/README.md`, "Gateway session (Step 6.2)". In short:

- `scripts/make_dev_certs.sh` writes the dev CA and gateway certificate to
  `.dev-certs/` (ignored by Git);
- `scripts/make_dev_env.ps1` writes `.env` (ignored by Git; it refuses to
  write otherwise) from the device id and the `token_sha256` digest that
  `provision.py` prints — `TTH_DEVICES` is single-quoted so the JSON survives
  `--env-file`;
- `deno task start:lan` starts the gateway with that `.env`. A variable set
  in the shell takes precedence over `.env`, which the device tests use for
  `FAKE_GEMINI_SETUP=fail` and a deliberately wrong `TTH_DEVICES`.

`FAKE_GEMINI_SETUP=fail` (connection refused → retryable error) and `never`
(the 15 s setup timeout) exercise the device's retry paths. The dev CA's
private key (`.dev-certs/ca.key`) never leaves this machine; only `ca.pem`
goes onto the robot.

### Activities, prompts and real Gemini on the LAN (final-v1 V5)

- **Per-device activity.** A registry entry's `activity_id` (a lowercase
  UUID, validated when the registry is parsed) selects the activity. It is
  loaded **fresh from Supabase on every device connection** with the
  publishable key only (the anonymous read policy; never `service_role`), as
  one row by id, with a bounded response. A device with `activity_id: null`
  keeps the default selection (the first enabled push-to-talk activity, else
  the first enabled one).
- **Fail closed before Gemini.** A malformed, missing or disabled activity, a
  prompt over 8 000 characters, more than 12 participants, an invalid
  participant name, a composed instruction over 12 000 characters, or a
  Supabase HTTP/network failure → HTTP 503 **before the WebSocket upgrade**:
  no session, no token mint, no Gemini connection.
- **Prompt.** `composeSystemInstruction` is the tested port of Flutter's
  composer (base prompt generated from the Dart source + activity prompt +
  participants sentence). The snapshot is frozen for the device session: a
  Gemini reopen inside the session reuses it; a device reconnect loads a new
  one.
- **Logs** carry only fixed event names, the device id, the activity UUID and
  bounded counters; the logger's field allow-list makes prompts, titles and
  child names unloggable.
- **Live mode on the LAN.** With `SUPABASE_URL`, `SUPABASE_PUBLISHABLE_KEY` and
  `GATEWAY_MINT_SECRET` in the ignored `.env`, start with
  `$env:GEMINI_MODE = "live"` in the shell (the `.env` default stays `fake`).
  The gateway holds no Gemini key: every session mints a single-use token
  through `gateway-gemini-token` and connects to
  `BidiGenerateContentConstrained`.

## Deployment — gated

**No public deployment until both gates are met:**

1. `gateway-gemini-token` is deployed with its secret, and the migration
   `supabase/migrations/20260911190000_gateway_mint_admit.sql` is applied;
2. `supabase/pending/20260911190100_restrict_activity_writes.sql` is applied
   (anonymous prompt writes disabled — this stops web editing until the web
   app signs in as an editor).

```bash
# Supabase (from the repo root)
supabase secrets set GATEWAY_MINT_SECRET=<32+ random chars> --project-ref <ref>
supabase db push                                   # applies the mint-admit migration
supabase functions deploy gateway-gemini-token --project-ref <ref>

# Cloud Run
gcloud secrets create GATEWAY_MINT_SECRET --data-file=-      # same value as above
gcloud secrets create TTH_DEVICES --data-file=devices.json   # device registry
gcloud run deploy tth-gateway --source gateway \
  --timeout=3600 \
  --set-secrets=GATEWAY_MINT_SECRET=GATEWAY_MINT_SECRET:latest,TTH_DEVICES=TTH_DEVICES:latest \
  --set-env-vars=SUPABASE_URL=https://<ref>.supabase.co,SUPABASE_PUBLISHABLE_KEY=<publishable>,TRUST_PROXY=1
```

Cloud Run ends WebSocket requests at the 60-min timeout, so the device
reconnects proactively at ~55 min, between turns; the gateway also sends
`session_end{max_age}` at 57 min.

## Device registry (`TTH_DEVICES`, a secret)

```json
{ "core2-01": { "token_sha256": "<sha256 hex of the device token>", "activity_id": null, "enabled": true } }
```

Device tokens are 32 random bytes, base64url (43 chars). Only their SHA-256
digest is stored here; the token itself goes onto the device by USB
provisioning. Revoking = `enabled: false` or removing the entry.
