# Phase 6 — networked conversation through the TTH gateway

Status: **architecture approved in principle; review corrections applied.
Step 6.0 implementation and automated verification accepted provisionally.
Real-Gemini barge-in MEASURED on the production sequence (manual realtime
activity): INTERRUPTS, 62 ms, turn 2 answered (§5.6.2). The clientContent
diagnostic is recorded separately (§5.6.1). Step 6.0 approved and complete.
Step 6.1 (USB-serial provisioning, NVS configuration, Wi-Fi link, Sleeping
face) COMPLETE: physical gates P1 and P2 passed on the Core2. Step 6.2 (TLS
WebSocket session: hello/ready, keepalive, backoff, auth rejection, proactive
reconnect) IMPLEMENTED and host-verified. First physical run: the gateway
path, keepalive and a local loopback turn PASSED, but the **M2 memory gate
FAILED during playback** (§7.1) → the face sprites moved to PSRAM (compile-time,
no fallback) → the **PSRAM memory retest PASSED** (§7.2): four gateway
sessions, current internal free ≥ 109 KB throughout, no audio or network
regressions, at a measured rendering-time cost. Step 6.3 (gateway turns) is
CLOSED on the accelerated v1 path: core behaviour physically confirmed, P5.5,
P7 and P14 skipped by product-owner decision, occasional playback underruns
accepted as a known issue (§11.2). Step 6.4 is replaced by the final-v1 plan
(under review). Nothing is deployed.**

Scope: Wi-Fi, provisioning, credential storage, a TLS WebSocket link to the
TTH gateway, live 16 kHz user-audio streaming through `GatewayTurnSource`,
24 kHz speech playback, connection/auth/timeout/retry state machines, the
offline and error faces, network barge-in, ordered and credit-controlled
queues, session and prompt lifecycle, a hardened token path, and the memory
measurements that settle the face-sprite question.

---

## 0. Decisions (reviewed)

| # | Decision |
|---|---|
| D1 | **LAN gateway** for development, **Google Cloud Run** for the hosted pilot, with the request timeout set to **60 min**. Cloud Run ends a WebSocket request at that timeout, so the device reconnects **proactively at ~55 min, between turns only**. |
| D2 | The gateway gets Gemini ephemeral tokens from a Supabase function **only once that path is authenticated** (§4.3). |
| D3 | `free_conversation` activities run as **push-to-talk** on the Core2; the mode conversion is logged. |
| D4 | Add the **Sleeping** face. Transient errors stay visible for at least 3 s, then recover to Ready or Sleeping. **AuthRejected** and **ProtocolMismatch** stay persistent. |
| D5 | **Gateway secret registry** of devices for the pilot. |
| D6 | Conversation context resets when Gemini reconnects (accepted for Phase 6). |
| D7 | Close the Gemini session after **5 min idle**. |
| D8 | **No** irreversible NVS/flash encryption for the pilot. |
| D9 | Activity selection is **server-side per device**. The serial next-activity command exists **only in a diagnostic build** and is absent from production builds (compile-time flag, tested). |
| D10 | Harden the Gemini-token path **as part of Phase 6**, before any public gateway deployment. |

Architecture (unchanged): **M5 → TTH gateway → Gemini Live.** The M5 never
holds the Gemini key, a Gemini token, a Supabase secret or prompt text.

```
 M5Stack Core2                      TTH gateway (Deno/TypeScript)                Supabase / Google
 ─────────────                      ────────────────────────────                ─────────────────
 PTT ─ capture ─ TurnBuffer         device auth (registry secret)
         │                          limits · rate limits · redacted logs
   TurnStreamer                     activity + prompt (generated base prompt) ──▶ `activities` (read)
         │  ordered outbound        per-turn state · credit · ordering
   OutboundQueue ──wss binary/text─▶ Gemini adapter (JSON + base64 here) ──wss──▶ Gemini Live
                                    token client ──HMAC-signed POST──────────▶ `gateway-gemini-token`
   down ring ◀── credit-gated ───── ordered downstream lane                      (GEMINI_API_KEY)
         │
   PcmPlayer
```

---

## 1. Facts this plan rests on

(From the first revision; still true.)

- `gemini-token`: `uses: 1`, 60 s to start, 30 min lifetime, locked to
  `models/gemini-3.1-flash-live-preview` + `AUDIO`. It runs with
  `verify_jwt = false` and is **unauthenticated**.
- **The Flutter app has no authentication at all:** Supabase is initialised
  with the publishable key, there is no sign-in anywhere, and anonymous
  sign-ins are disabled in `config.toml`. The web activity form **writes
  prompts as `anon`**.
- `activities` RLS grants `anon` full select/insert/update/delete.
- `composeSystemInstruction` = base prompt + activity prompt + ordered
  participants, joined by blank lines. The Gemini PTT setup uses the Puck
  voice and `automaticActivityDetection.disabled = true`.
- Flutter PTT ignores presses while speaking, so **PTT barge-in on Gemini is
  unproven** (checked in 6.0).
- Device: `esp_websocket_client` (own task; supports `headers`,
  `subprotocol`, `cert_pem`, `disable_auto_reconnect`, ping/pong; **no
  CA-bundle attach**); sends block with a timeout; mbedTLS allocates from
  **internal RAM** with 16 KB records; `HAVE_TIME` without `HAVE_TIME_DATE`
  (**certificate validity dates are not checked**).
- Supabase Edge Functions have a per-invocation wall-clock limit, so the
  relay cannot live there (option B from the first revision stays rejected).

---

## 2. Protocol `tth.v1` (revised)

WSS, subprotocol `tth.v1`. Upgrade headers: `Authorization: Bearer <device
token>`, `X-TTH-Device: <device id>`.

### 2.1 Binary frames — audio only, 8-byte header

```
offset  size  field
0       1     kind      0x01 user audio (device→gateway) · 0x02 model audio (gateway→device)
1       1     flags     0 (reserved; non-zero → protocol error)
2       2     reserved  0 (non-zero → protocol error)
4       4     turn_id   uint32 little-endian; 0 is RESERVED and invalid
8       n     PCM       s16le mono · 16 kHz up · 24 kHz down · n even
```

- Up: n = 640 B (20 ms) for every frame except the last of a turn (1…640 B).
- Down: 1 ≤ n ≤ 1 920 B (40 ms at 24 kHz, one `PcmPlayer` slot), so a
  complete frame (≤ 1 928 B) always fits the device's 2 048 B WS buffer.
- Any other kind, odd n, n = 0, oversize, or turn_id 0 → protocol error.

### 2.2 Turn IDs (uint32)

- Allocated by the **device**, starting at 1 and incrementing; `0xFFFFFFFF`
  wraps to **1** (0 is never used).
- A new id must differ from the active turn **and** from the last
  **16** ended or cancelled turns (a recent set); the allocator skips any
  collision. At one turn per second, a 32-bit counter takes 136 years to
  wrap, but the skip rule is implemented and tested anyway.
- The gateway rejects `turn_start` with id 0, the active id, or an id in its
  own recent set → `error{code:"bad_turn"}`. Any frame for an unknown or
  finished turn is dropped **and counted**.

### 2.3 Control frames — JSON text, ≤ 512 B (both sides enforce)

| Dir | Message | Notes |
|---|---|---|
| D→G | `hello {proto:1, fw, in:"s16le/16000/1", out:"s16le/24000/1", maxDown:1920, credit:C}` | `C` = initial downstream credit in PCM bytes |
| G→D | `ready {session, activity, out}` | Gemini `setupComplete` received |
| D→G | `turn_start {turn}` | ordered lane |
| D→G | `turn_end {turn, frames, bytes}` | ordered lane; totals let the gateway **verify** it received every frame |
| D→G | `cancel {turn}` | priority, with purge semantics (§3.2) |
| D→G | `credit {bytes}` | priority; returns consumed downstream credit |
| G→D | `speech_start {turn, fmt:"s16le/24000/1"}` | ordered downstream lane |
| G→D | `turn_complete {turn, frames, bytes}` | ordered, after the turn's last audio frame |
| G→D | `interrupted {turn}` | ordered, informational |
| G→D | `error {code, retry, turn?}` | turn-scoped → ordered lane; connection-scoped → priority |
| G→D | `session_end {reason}` | `replaced` (a newer connection from the same device), `max_age` (reconnect before Cloud Run's 60 min). Gemini idle close, token expiry and `goAway` are handled inside the gateway, between turns, and are not signalled to the device |
| both | `ping {ts}` / `pong {ts}` | priority |

The device parses control frames with a fixed-schema parser on fixed buffers
(no heap). Unknown `t` → ignored and counted. Malformed or oversized →
protocol error.

---

## 3. Ordering and flow control

### 3.1 Upstream: one ordered outbound queue with a send fence

The earlier design had separate audio and control queues, which allowed
`turn_end` to overtake queued audio. **Replaced by one `OutboundQueue`**
(portable, `lib/tth_core`) holding complete frames (header + payload) in two
lanes:

| Lane | Contents | Order |
|---|---|---|
| **ordered** | `turn_start(N)`, `audio(N)…`, `turn_end(N)` | strict FIFO |
| **priority** | `credit`, `ping`, `pong`, `cancel` | FIFO among themselves; sent before the next ordered item, **only at frame boundaries** |

**The fence is structural.** There is exactly one sender (the `NetSender`
task). It takes the head item, calls `esp_websocket_client_send_*`, and
**removes the item only after the full length was sent successfully**.
`turn_end(N)` is behind every audio frame of turn N in the same FIFO, so it
is transmitted only after all of them were sent — not merely queued. A send
that fails or times out means the connection is failing: the queue is
cleared, the turn fails, and the link state machine reconnects.

**Guaranteed wire order for a turn:**

```
turn_start(N) · audio(N)₁ … audio(N)ₖ · turn_end(N, frames=k, bytes=Σ)
```

(Priority frames — credit, ping — may appear between items, never inside
one.)

**Backpressure:** `pushUserAudio()` copies one 640 B frame into a free slot,
or returns `Busy` (TurnStreamer retries; the TurnBuffer holds the audio —
the Phase 5 lossless path). `turn_start`/`turn_end` use reserved capacity so
they can never be starved by audio.

### 3.2 Cancel: defined ordering and priority

`cancel(N)` is a priority item, **processed by the sender at the next frame
boundary**:

1. every **unsent** ordered item of turn N (`turn_start`, audio, `turn_end`)
   is purged;
2. if `turn_start(N)` itself was never sent, the gateway never heard of N:
   the cancel is dropped (nothing to cancel);
3. otherwise `cancel(N)` is sent **before any item of turn N+1**, and after
   whatever of N was already on the wire.

So on the wire nothing of turn N ever follows `cancel(N)`, and `cancel(N)`
always precedes `turn_start(N+1)`. The gateway, on `cancel(N)`: stops
forwarding N to Gemini, purges N's unsent downstream items (their credit was
never consumed), and drops (and counts) any late N frame.

### 3.3 Downstream: application-level credit (no blocking anywhere)

The earlier "WS task waits while the down ring is full" design is
**withdrawn**. The WS event task never waits for the playback ring.

- `hello.credit` = the down ring's free PCM capacity (planned: 192 000 B,
  4 s). This is the gateway's initial **credit**.
- The gateway sends a model-audio frame **only if** its PCM length ≤ the
  current credit, and subtracts it. It keeps un-creditable audio in its own
  per-turn queue (bounded, §4.6).
- The device returns credit with `credit{bytes}` after audio is **consumed**
  — copied by `PcmPlayer::submit()` — batched at ≥ 3 840 B (80 ms) or at
  turn end. Frames discarded as stale (cancelled turn) also return their
  credit, so **cancellation never leaks credit**.
- **Control frames never consume credit** and are always deliverable:
  `turn_complete(N)` waits behind N's audio in the ordered lane, but `pong`,
  `session_end` and connection errors go on the priority lane even at zero
  credit.
- **Conservation invariant**, checked on the device:
  `bytes received − bytes returned ≤ credit granted`. A frame that would
  break it, or that does not fit the ring, is a **credit violation**: the
  frame is dropped explicitly, `creditViolations++`, the turn fails
  (`Error(protocol)`, `cancel(N)` sent), and the connection is
  re-established, because after a violation the two sides' credit counts
  can no longer be trusted. The WS task only increments a counter and posts
  an event — it never waits.
- A device returning more credit than is outstanding is a protocol error at
  the gateway (connection closed, counter logged).

---

## 4. Security design

### 4.1 Device credentials

- NVS namespace `tth`: `ssid`, `pass`, `url` (`wss://` only), `dev_id`,
  `dev_tok` (32 random bytes, base64url), `ca_pem` (optional override).
- Provisioned over USB serial (`tools/provision.py`: prompts or environment
  variables, never a repo file). The firmware never echoes the password or
  token; every log line redacts them.
- **Not encrypted at rest** (D8): anyone with physical access who dumps the
  flash obtains the Wi-Fi password and that robot's token. The token only
  grants "talk as this robot" and is **revocable** in the gateway registry.

**As implemented in Step 6.1** (refines the list above):

- One namespace `tth`, but the five fields are stored together as a complete
  record in two slots, `cfgA` and `cfgB`, not as one key per field — so a
  commit can never leave a mix of old and new values. Record: `"TTHC"`,
  version 1, generation (u32), five length-prefixed fields (`ssid`, `pass`,
  `url`, `id`, `token`), CRC-32. A commit writes the slot that does **not**
  hold the live record, reads it back, decodes and re-validates it, and only
  then switches; boot takes the valid slot with the highest generation. An
  interrupted or corrupted write therefore always leaves the last valid
  configuration in use (`lib/tth_core/src/ConfigStore.cpp`, tested with
  failed, torn and corrupted writes).
- Validation before anything is staged, and again on load: SSID 1–32 bytes
  without control characters; password empty (open network, confirmed by the
  tool), 8–63 printable ASCII or 64 hex; URL `wss://` only (`ws://` and every
  other scheme refused), DNS or IPv4 host, optional port, path required, no
  user info / query / fragment, ≤ 200 bytes; device id and token with the
  gateway's own rules (`DEVICE_ID_RE`, `DEVICE_TOKEN_RE`).
- `WiFi.persistent(false)`: the Arduino Wi-Fi library would otherwise copy
  the SSID and password into the Wi-Fi driver's own NVS namespace, out of
  reach of a TTH Bot reset. `WiFi.setAutoReconnect(false)`: reconnection
  belongs to the link state machine (§8).
- Reset: `!prov reset` issues a four-digit code; `!prov reset confirm <code>`
  within 30 s erases namespace `tth` only (`Preferences::clear()` on that
  handle). A wrong or expired code cancels.
- Commit and reset are refused while a turn, capture, stream, playback or
  barge-in is active; serial input is consumed at most 256 bytes per loop.
- `ca_pem` is deferred to Step 6.2, where the TLS client that uses it lands.

**Provisioning method for v1 (decided):** USB serial through
`tools/provision.py` is the supported method. The Wi-Fi credentials, gateway
URL, device identity and CA are data in NVS, so changing them never requires
reflashing: the Core2 is connected to a laptop and `provision.py` is run again
(`--set-wifi` changes only the SSID and password, `--set-url` only the URL,
`--ca-only` only the CA; the device token and registry entry stay valid).
Firmware is uploaded only when firmware code changes. The transactional NVS
storage and the recovery procedure above stay as they are. Phone-based
provisioning (BLE, SoftAP, a captive portal or the Flutter app) is **not**
implemented in Phase 6 and is only a possible post-v1 enhancement (§15).

### 4.2 TLS and certificates — corrected wording

- The device verifies the gateway certificate against a **pinned CA** (the
  `cert_pem` config) **and verifies the hostname** (the CN/SAN check stays
  enabled: `skip_cert_common_name_check = false`).
- **This build does not check certificate validity dates.** That is a real
  gap, and **short certificate lifetimes do not compensate for it**: a
  device that ignores dates accepts an expired certificate regardless of how
  short-lived it was meant to be. What limits the exposure is:
  - the pinned CA plus hostname verification (only a certificate chaining to
    the pinned CA, for the right host, is accepted);
  - revocable device tokens (a stolen token is cut off at the gateway);
  - a documented **CA/certificate rotation path**: the firmware carries the
    pinned root(s) — for Cloud Run the Google Trust Services root plus one
    backup root — and `ca_pem` in NVS can be replaced by re-provisioning, so
    a rotated or compromised root can be changed without a firmware rebuild.
- LAN development gateway: its own dev CA, pinned the same way. Its private
  key never enters the repo.

**As implemented in Step 6.2** (refines the above and §1's client choice):

- **Client:** `WiFiClientSecure` (Arduino core mbedTLS) plus a small portable
  RFC 6455 codec (`lib/tth_core/…/WebSocketCodec`), in one FreeRTOS task on
  core 0 (`src/net/GatewayClient`). Not `esp_websocket_client`: the IDF 4.4
  client in this core does not expose the HTTP status of a failed upgrade,
  so a 401 (AuthRejected, persistent per D4) could not be told from a network
  error. The codec checks the 101 strictly (Upgrade, Connection,
  Sec-WebSocket-Accept, subprotocol `tth.v1`) and reports any other status.
- **Trust anchor:** the CA bundle (≤ 4 KB, ≤ 3 certificates, public) is
  provisioned over USB serial (`!prov ca …`, `provision.py --ca/--ca-only`)
  into NVS as a separate A/B record (`caA`/`caB`, "TTHA", generation, CRC-32),
  structurally checked and parsed by mbedTLS (every certificate must be a CA)
  before it is committed. **Without a CA the device does not connect** —
  there is no insecure mode. The built-in Google Trust Services roots for the
  hosted pilot are **not** compiled in yet: nothing is deployed, and their
  exact bytes must be verified when the pilot is prepared.
- **Verification:** `VERIFY_REQUIRED` against the pinned CA plus the host name
  (`mbedtls_ssl_set_hostname`), and the verification result is read again
  after the handshake. This mbedTLS build matches the host against DNS
  subjectAltNames only, so a LAN certificate for an IP address carries it as
  `DNS:<ip>` as well as `IP:<ip>` (`gateway/scripts/make_dev_certs.sh`).
  Certificate dates remain unchecked (the residual gap above).
- **Token hygiene:** the upgrade request buffer (which carries the bearer
  token) is zeroed right after it is written; the token is never logged.

### 4.3 Token minting — option B

Option A (user JWTs for Flutter plus a gateway credential) would require
adding Supabase Auth to the Android app, which has none today. That is
breaking, so it is left as a separately reviewed future change. **Option B
is implemented:**

- A new Edge Function, **`gateway-gemini-token`**, used only by the gateway.
  The existing `gemini-token` stays unchanged for the Flutter app (its
  residual risk is documented and pre-existing; the gateway never uses it).
- **Credential: an HMAC-signed, short-lived assertion**, so the secret is
  never transmitted:
  `x-tth-gateway-auth: ts=<unix ms>,nonce=<16 B base64url>,sig=<base64url HMAC-SHA256(secret, "tth-gateway-mint\n<ts>\n<nonce>")>`
  - missing or malformed → 401; wrong signature → 401 (constant-time
    compare); `|now − ts| > 60 s` → 401 (**expired** or future); a
    **replayed nonce** → 401.
  - `GATEWAY_MINT_SECRET` lives in the Supabase function secrets and in
    **Cloud Run Secret Manager** — never in Git, never in firmware.
- **Rate limit + replay store:** one Postgres function,
  `gateway_mint_admit(nonce, limit, window_s)`: an atomic insert of the
  nonce (unique) plus a count of mints in the window. It is `SECURITY
  DEFINER`, executable **only by `service_role`**, and returns
  `ok` / `replay` / `rate_limited`. The function calls it with the service
  key the Edge runtime provides.
- On success it mints a one-use Gemini token with the **same constraints** as
  `gemini-token` (model and `AUDIO` locked, `uses: 1`).
- `verify_jwt = false` for this function: it enforces its own HMAC
  authentication (a JWT would need the powerful service-role key on the
  gateway).

### 4.4 Activities RLS — prompt mutation needs an authorised role

The gateway turns activity prompts into **system instructions**, so
anonymous writes are not acceptable once it is used beyond a trusted LAN.

- Migration (written in 6.0, **not applied**): drop the `anon`
  insert/update/delete policies; keep `anon` select; allow writes only to
  `authenticated` users whose **`app_metadata.role = 'activity_editor'`**
  (set by an admin through the service role). A plain sign-up is **not**
  enough — `enable_signup = true` in `config.toml` would otherwise let anyone
  become `authenticated`.
- **Consequence:** the web activity form writes as `anon` today, so applying
  the migration disables web editing until the web app signs in as an editor
  (a Flutter web change). Applying it early is fail-safe: reads and the
  Android app are unaffected; only prompt editing stops.
- **Gate:** the gateway must not be deployed publicly (Cloud Run) before this
  migration is applied. Until then it runs on the LAN only.

### 4.5 Gateway authentication and rate limits

- Device token check: SHA-256 of the presented token vs. the registry digest,
  **constant-time**, with equal work for unknown devices.
- Rate limits (token buckets, in memory, per gateway instance):
  - connection attempts per IP: 10/min;
  - connections per device: 6/min;
  - `turn_start` per device: 30/min;
  - 5 failed authentications from an IP → blocked for 5 min.
- One live session per device; a new connection replaces the old one
  (`session_end: replaced`).

### 4.6 Explicit gateway limits

| Limit | Value | On violation |
|---|---|---|
| activity prompt length | 8 000 chars | activity refused: `error{activity_invalid}` (never truncated) |
| composed system instruction | 12 000 chars | same |
| participant count | 12 | same |
| participant name length | 40 chars; letters incl. Romanian diacritics, space, `-`, `'` | same |
| control frame size | 512 B | protocol error, close |
| binary frame size | up: header + ≤ 640 B · down: header + ≤ 1 920 B | protocol error, close |
| user audio per turn | 1 472 000 B (46 s at 16 kHz; matches the device turn buffer) | `error{turn_too_long}` + the turn is cancelled |
| model audio per turn (gateway queue) | 5 760 000 B (120 s at 24 kHz) | response truncated, `error{response_too_long}` |
| turns / connections / auth failures | §4.5 | throttled / 429 / blocked |

### 4.7 Logging

Structured JSON with an **allow-list of fields**: device id, session id, turn
id, activity **id** (never title or prompt), counters, byte and frame totals,
latencies, error codes. **Never** audio, prompt text, participant (child)
names, device tokens, Gemini tokens or secrets. A redaction test feeds every
log call site sentinel values and asserts none reach the output.

---

## 5. Gateway (`gateway/`, Deno)

**Per connection:**
1. **Auth** (§4.5) → 401 before the upgrade on failure.
2. **Activity:** the registry's `activity_id`, else the first enabled
   `push_to_talk` activity ordered `sort_order, title`. A `free_conversation`
   activity is run as PTT and **logged as a mode conversion** (D3). Limits
   are checked (§4.6).
3. **Prompt:** `composeSystemInstruction` ported to TypeScript. The base prompt
   comes from **`gateway/src/generated/edubot_base_prompt.ts`**, generated
   **at build time** by `scripts/generate_base_prompt.ts` from
   `lib/core/prompts/edubot_base_prompt.dart`. There is **no Dart parsing at
   runtime**. The generator fails on any Dart literal construct it does not
   handle (string interpolation `$`, escapes `\`, raw or adjacent strings) —
   which forces a review. **A parity test fails if the generated file does
   not match the current Dart source**, and compares the composed output for
   the seed activities with the Dart composer's rules.
4. **Gemini:** mint through `gateway-gemini-token` (§4.3). Setup identical to
   Flutter's PTT setup. 15 s `setupComplete` timeout.
5. **Turns:** the ordered upstream mapping `turn_start → activityStart`, audio
   → base64 `realtimeInput.audio` (16 kHz), `turn_end → activityEnd` (after
   checking `frames` and `bytes`). Downstream: Gemini audio → decode →
   ≤ 1 920 B chunks → the ordered lane `speech_start · audio… ·
   turn_complete`, audio released only against credit (§3.3).
6. **Cancel:** §3.2, with the interrupt **fence**: after `cancel(N)` every
   model-audio chunk is dropped (and counted) until Gemini acknowledges with
   `interrupted` or `turnComplete`, or 5 s pass. When `interrupted` lowers
   the fence, the next `turnComplete` that arrives before any new-response
   audio **closes the interrupted response** and is consumed — it never ends
   the next turn, even if it arrives after that turn's `turn_end`. The next
   `activityStart` interrupts Gemini on the production path (measured,
   §5.6.2), so the bounded fallback (hold the new turn's upstream audio
   ≤ 10 s until the old turn's `turnComplete`/`interrupted`) is **not
   needed**; it stays documented as a contingency only.
7. **Lifecycle:** Gemini closed after 5 min idle (D7) and reopened on the next
   `turn_start`. Reconnect before the 30-min token expiry, or on `goAway`,
   **between turns only**. Cloud Run's 60-min request timeout (D1) is handled
   by the device's proactive reconnect at ~55 min (§8); the gateway also
   emits `session_end{max_age}` at 57 min if the device has not reconnected.
8. **Send backpressure:** the gateway stops dequeuing when the socket's
   `bufferedAmount` exceeds 64 KB.

**Deployment:** a container (`gateway/Dockerfile`, Deno), Cloud Run request
timeout 3600 s, secrets from Secret Manager (`TTH_DEVICES`,
`GATEWAY_MINT_SECRET`, Supabase URL and publishable key). Public deployment
is gated on §4.3 (deployed) and §4.4 (applied).

### 5.6 Barge-in on the real Gemini

Two probes, kept apart because they test different things. Both used a
temporary developer key locally; nothing was deployed, and neither printed or
stored the credential.

#### 5.6.1 Diagnostic: `clientContent` (a TEXT turn)

`gateway/manual/barge_in_probe.ts`. The long response is requested with
`clientContent`; the barge-in is `activityStart` + 0.5 s audio +
`activityEnd`. **Not the gateway's path** — the first turn is text — so it
shows only that Gemini *can* interrupt.

| Measure | Result |
|---|---|
| verdict | INTERRUPTS |
| barge-in `activityStart` → `interrupted` | 70 ms |
| old audio received after the barge-in | 280 ms |

#### 5.6.2 Production sequence: manual realtime activity — MEASURED

`gateway/manual/barge_in_live_probe.ts`. Only the production configuration
and message order: `automaticActivityDetection.disabled = true`; each user
turn is `activityStart` · 16 kHz mono s16le `realtimeInput.audio` in 20 ms
frames paced in real time · `activityEnd`. Turn 2 is sent while response 1
is streaming (≥ 1.5 s of audio received). Input: synthesised.

| Measure | Result |
|---|---|
| verdict | **INTERRUPTS** |
| second `activityStart` → `interrupted` | **62 ms** |
| old response-1 audio after the second `activityStart` | **240 ms** of audio, all within those 62 ms |
| audio within 300 ms after `interrupted` | **0 ms** |
| audio between `interrupted` and turn 2 `activityEnd` | 0 ms |
| `turnComplete` closing response 1 | 66 ms after `interrupted`, before turn 2 `activityEnd`, before any new audio |
| turn 2 received a valid new response | **yes** |
| response 2 first audio | 1013 ms after turn 2 `activityEnd` |
| response 2 audio received | ≥ 33,161 ms (still streaming when the probe stopped listening) |
| response 1 first audio | 793 ms after turn 1 `activityEnd` |

Raw timeline (ms from connect):

| ms | event |
|---:|---|
| 751 | `setup_complete` |
| 770 / 2547 | turn 1 `activityStart` / `activityEnd` |
| 3340–3825 | response 1 audio (1611 ms before the barge-in, 240 ms after) |
| 3763 | turn 2 `activityStart` (barge-in) |
| 3825 | `interrupted` (after the last response-1 chunk, same millisecond) |
| 3891 | `turnComplete` — closes response 1 |
| 4540 | turn 2 `activityEnd` |
| 5553–14553 | response 2 audio, 33,161 ms |

**Correction to the first report of this run.** That report said
`secondTurnGotResponse: false` and `secondTurnResponseMs: 0` while a
"generation 3" held 33,161 ms. This was a probe bug, not Gemini behaviour: the
probe advanced a generation on `interrupted` **and** on the `turnComplete`
that closes the interrupted response, so response 2 was labelled generation 3
and never counted. The probe now attributes audio from the locally sent turn
boundaries and the arrival order of every chunk and event
(`gateway/manual/probe_attribution.ts`); arrival order matters because the
last old chunk and `interrupted` shared a millisecond.
`gateway/tests/probe_attribution_test.ts` rebuilds this run from its recorded
timestamps and asserts every corrected figure. The three interruption
measurements (62 ms, 240 ms, 0 ms) are unchanged by the correction.

**Consequences for the design:**
1. Barge-in works on the production path: **no fallback needed**.
2. Gemini closes an interrupted response with `interrupted` **and** a
   `turnComplete`. Replaying the measured timing exposed a gateway race: if
   that `turnComplete` reached the gateway after the next turn's `turn_end` (a
   barge-in utterance shorter than ~130 ms), it was taken as the new turn's
   end and the new response's audio was dropped as stale. Fixed in
   `gateway/src/session.ts` (§5 item 6; counted as
   `closedInterruptedResponses`) and tested in both orders.
3. Gemini delivered 33.2 s of audio in 9.0 s of wall time (~3.7× real time)
   and 240 ms in 62 ms. The device plays in real time, so downstream credit
   (§3.3) is a necessity, not a precaution.

Caveat: one run, synthesised input. The fence (5 s timeout) and the
closing-`turnComplete` rule hold regardless of timing.

#### 5.6.3 The fence, proven offline

`gateway/tests/fence_replay_test.ts` replays the measured production timing
through the real `DeviceSession`: response 1 streaming → `cancel(1)` + turn 2
upload → **240 ms of old audio over 62 ms** → `interrupted` → the closing
`turnComplete` → response 2. The device is given exactly enough credit for
the audio that should reach it and returns none. It proves that every
old-response chunk after the barge-in is dropped at the session boundary, is
counted (`staleModelFrames` = 6), **never becomes a downstream item, never
reaches the device and consumes no credit** (response 2 still arrives in
full), and that the closing `turnComplete` does not end turn 2 — in the
observed order and in the race order. The clientContent timing and a far
harsher timing (1.2 s of old audio over 1.5 s) are replayed too.

---

## 6. Device design (steps 6.1–6.4; unchanged except where noted)

| Where | Component | Change in this revision |
|---|---|---|
| `lib/tth_core` | `GatewayProtocol` | 8-byte header, uint32 turn ids, `credit`, counted `turn_end`/`turn_complete`, 512 B control limit |
| `lib/tth_core` | `TurnIdAllocator` | **new**: skip 0, recent-set collision rule |
| `lib/tth_core` | `OutboundQueue` | **new**: replaces the up ring **and** the control-out queue (§3.1–3.2) |
| `lib/tth_core` | `DownstreamCredit` | **new**: grant / receive-check / consume / batched return; violation counter (§3.3) |
| `lib/tth_core` | `GatewayTurnSource`, state machines, `Backoff`, `ProvisioningRecord` | as before |
| `src/net` | `WifiLink`, `GatewayClient`, `NetSender`, `CredentialStore`, `SerialProvisioning` | the WS task never waits; the `NetSender` is the only sender |

Buffers (preallocated, explicit placement):

| Buffer | Size | Region | Full → |
|---|---:|---|---|
| OutboundQueue | 32 slots × 656 B ≈ 21 KB | PSRAM | `Busy` (lossless) |
| Down ring | 192 000 B PCM + frame headers | PSRAM | cannot overflow within credit; beyond credit = violation |
| Control-in queue | 16 × 64 B | internal | newest refused + counted |
| WS rx buffer | 2 048 B | internal | frames ≤ 1 928 B by protocol |

The diagnostic build flag `TTH_DIAGNOSTIC_BUILD` compiles in the serial `n`
(next activity) command; production builds exclude it at compile time. A
host test verifies the command parser does not know `n` without the flag.

### 6.1 Step 6.3 as built

Decisions applied (review of the Step 6.3 scope):

1. **Barge-in** uses the local sequence: stop playback → purge the turn's
   buffered downstream audio and return its credit → queue `cancel` through
   the ordered path → start the microphone → LISTENING. No network or Gemini
   acknowledgement is awaited; late audio and events of the cancelled turn
   are dropped and counted. Real-Gemini interruption stays in 6.4.
2. **First-response timeout (10 s) only.** Armed when the NetSender reports
   `turn_end` *completely written* (`IUplink::lastTurnEndSent`, published
   after `completeSend`), not when queued. Satisfied only by the active turn:
   its first audio after `speech_start`, or a `turn_complete` with no audio.
   On expiry the turn fails once (`ResponseTimeout`): cancel queued, ring
   purged, credit returned, capture/playback stopped, AudioBus released,
   ERROR for 3 s, connection kept.
3. **Mock only in the diagnostic build.** `LocalMockTurnSource` and the keys
   `m p x g t` are compiled only with `TTH_DIAGNOSTIC_BUILD`; production always
   uses `GatewayTurnSource`.
4. **No real Gemini, no deployment.** Physical tests use `GEMINI_MODE=fake`.
5. **Fake audio** = the user turn echoed back, resampled 16→24 kHz in the
   gateway (`gateway/src/resample.ts`, linear, output `floor(n·3/2)` samples,
   bounded by `maxUserAudioBytesPerTurn`), `FAKE_ECHO_REPEAT` times.
6. **Thread ownership.** The loop produces outbound items and consumes the
   ring and events; the network task consumes outbound items and produces
   ring frames and events. `GatewayTurnSource` is loop-only and never called
   from the network task. The OutboundQueue is shared under a mutex held only
   for queue operations (never I/O); a slot is copied completely before the
   lock is released. The ring (`DownstreamRing`) is SPSC and lock-free: a
   record is staged past the published end and becomes visible only when the
   end is published.
7. **Outbound ordering** as §3.1–3.2; a failed or partial write → `failSend`,
   the connection is dropped, the turn fails once (session loss).
8. **Downstream ring and credit.** 192 000 B PCM + 8 192 × 12 B frame headers
   = 290 304 B, `heap_caps_calloc(MALLOC_CAP_SPIRAM)`, region-checked, no
   DRAM fallback (the gateway is disabled instead). Frames ≤ 1 920 B
   (960 samples, 40 ms, one playback slot); credit returned in 3 840 B
   batches (two frames), committed only after the `credit` message was
   queued. Conservation per connection:
   `granted = gatewayRemaining + inRing + (consumed − returned)`; audio held by
   PcmPlayer is consumed audio. The producer checks capacity, then credit, and
   never waits; a frame beyond either is refused without touching earlier
   data, counted (`creditViolations`), reported, and the connection closed
   (1008) → the turn fails once → reconnect with fresh epochs (the loop
   empties the ring and restarts its epoch *before* queuing the connect).
9. **Session loss** — one rule in `App::syncTurnSourceWithSession`: whenever
   the session leaves READY the active turn fails once (`SessionLost`);
   capture and playback stop; ERROR for 3 s (`onTransientError`), then the
   rest state for the link (Sleeping until reconnected, then Ready).
10. **TurnBuffer lease** unchanged: the streamer holds it while committed
    audio is unsent, including under `Busy` (host-tested).
11. **Fake-gateway controls** (fake mode only; `loadConfig` refuses any
    `FAKE_*` otherwise): `FAKE_ECHO_REPEAT` (1–20), `FAKE_RESPONSE_DELAY_MS`,
    `FAKE_FIRST_RESPONSE_DELAY_MS` (first turn of the process only),
    `FAKE_CREDIT_HOLD_MS` (returns held once credit is exhausted),
    `FAKE_VIOLATE_CREDIT=1` (one frame beyond credit per process).
    The gateway also now purges a *completed* response still queued behind
    credit when the device cancels it, so a barge-in never keeps streaming it.
12. **M3/M4** points logged as `[mem] M3/M4 <point>` (§7.3).
13. **Counters** in `[turn]` and `[credit]` heartbeat lines; no audio, prompt,
    token or credential is printed.

| Buffer (Step 6.3) | Size | Region |
|---|---:|---|
| Downstream ring | 290 304 B | PSRAM (checked) |
| OutboundQueue (32 × 660 B slots + indices; exact size printed at boot) | ≈ 21.7 KB | PSRAM (checked) |
| `GatewayTurnSource` scratch (one 960-sample chunk) | 1 920 B | `.bss` |
| Diagnostic mock synth scratch | 1 920 B | internal heap, **diagnostic build only** |

---

## 7. Memory measurement and the sprite decision

Unchanged: M0 (Phase 5 baseline) … M5 (30-min soak). Sprites stay in DRAM
only if, across M1–M5, minimum free internal ≥ 32 KB, largest free internal
block ≥ 24 KB, and zero TLS allocation failures over ≥ 20 reconnects.
Otherwise `TTH_FACE_SPRITES_IN_PSRAM` is set and M2–M5 are re-measured.
Decided in 6.2/6.3 on measurements.

### 7.1 Step 6.2 first physical run — the memory gate failed

Sprites in internal DRAM (81 112 B), Wi-Fi up, one TLS WebSocket session,
`tlsAllocFail=0` throughout:

| Point | internal free | largest block | historical min |
|---|---:|---:|---:|
| M1, after Wi-Fi | 73 496 | 69 620 | 70 080 |
| during TLS / WebSocket connect | 31 356 | 30 708 | 28 444 |
| M2, gateway ready #1 | 36 328 | 28 660 | 26 252 |
| **during playback (speaking)** | **28 472** | **25 588** | 24 348 |
| after playback, idle | 36 100 | 28 660 | 24 348 |
| later, idle | 36 100 | 28 660 | **12 140** |

**Current free vs historical minimum.** `free` and `largest` are the heap
*now*; `min` is the lowest `free` ever reached since boot (a monotonic
low-water mark). The thresholds apply primarily to the **current** figures at
each operating point; the minimum shows that a transient allocation happened,
not that memory is leaking.

**Verdict.** Idle at M2 passes (36 328 ≥ 32 KB, 28 660 ≥ 24 KB). Normal
playback **fails**: current free 28 472 < 32 KB, and the largest block 25 588 is
only ~1.6 KB above 24 KB — no usable margin. The gate therefore fails.

**The 12 140 B minimum.** It appeared while idle after playback: `free` and
`largest` returned to 36 100 / 28 660 afterwards, so it was a transient
allocation of roughly 12 KB, **not shown to be a leak**. The existing
instrumentation cannot attribute it: the heartbeat samples the monotonic
minimum every 10 s and records no cause, and no turn was active. In this
Arduino core the Wi-Fi driver's dynamic RX buffers (up to 32) and lwIP buffers
are allocated from internal RAM (`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` is
off), so a burst of incoming LAN traffic is a plausible cause — a hypothesis,
not a finding. Narrow instrumentation now attributes it:

- the loop reads the low-water mark at six checkpoints and logs
  `[mem] new internal low-water N B (-drop) now free=… largest=… | state=…
  audio=… gw=… wifi=… | first seen after: <stage>` whenever it falls ≥ 2 KB.
  The first checkpoint ("yield (wifi/tls/audio tasks), M5.update") covers
  everything other tasks did while the loop yielded;
- every gateway connect reports `low-water before->after` for DNS, TCP, TLS
  and the upgrade.

The checkpoints' own cost appears as `mem.checkpoint` in `[blocks]`.

**Decision (the §7 rule).** Both face sprites move to **PSRAM**, fixed at
compile time (`TTH_FACE_EYES_IN_PSRAM=1`, `TTH_FACE_LOWER_FACE_IN_PSRAM=1`).
The renderer allocates exactly there, reads the buffer address back, and
refuses to render if it is anywhere else — the former "DRAM first, PSRAM on
allocation failure" fallback is removed. A hybrid (one sprite in DRAM) was not
chosen because no measurement shows it meeting every threshold during TLS,
idle, capture, playback and reconnect with margin; it can be measured later
by changing one flag.

**Expected effect** (to be confirmed by the retest):

- internal free rises by about 81 KB at every point: M1 ≈ 154 KB,
  M2 ≈ 117 KB, playback ≈ 109 KB; the largest block should grow as well (the
  eyes sprite alone was a 60 320 B allocation), but fragmentation decides by
  how much;
- the historical minimum rises by about the same amount, unless the
  unexplained transient recurs larger;
- **rendering is slower.** Measured when the sprites were last in PSRAM: a
  full-face transition took **25.5 ms** against a 16.2 ms SPI floor (PSRAM
  bandwidth ~9 ms for ~162 KB touched at ~17 MB/s). Estimated from the same
  rate: eyes-only frames (blinks, most transitions) ≈ 12 ms SPI + ~7 ms PSRAM
  ≈ 19 ms; cheek-bar frames while speaking ≈ 4 ms SPI + ~2.5 ms PSRAM. So
  `maxTrans` is expected near 25 ms, `maxPush` and `maxLoopSteady` near 20 ms
  on blink frames, and `maxLoopSpeaking` a few ms higher than with DRAM.
  `TTH_LOOP_WARN_MICROS` stays at 20 ms: `face render` alerts and
  `steady-state loop exceeded` lines are reported findings, not hidden. The
  retest must also show that capture and playback stay intact (no failed or
  dropped samples, no underruns) with the slower frames.

### 7.2 Step 6.2 PSRAM retest — PASSED

Production firmware, both face sprites in PSRAM. The heartbeat reports
`sprites=PSRAM`; the face stayed functional and visually correct.

| Point | internal free | largest block |
|---|---:|---:|
| M1, after Wi-Fi | ≈ 154 776 | ≈ 110 580 |
| gateway READY | ≈ 117 568 – 117 580 | 94 196 |
| during playback | ≈ 109 728 | 94 196 |
| after playback | ≈ 117 368 | 94 196 |

Compared with §7.1: playback free rose from 28 472 to ≈ 109 728 B and the
largest block from 25 588 to 94 196 B — far above the 32 000 B and 24 000 B
thresholds. The **historical minimum** stayed around 85–100 KB in the later
tests, with one reported value of **90 068 B** after repeated reconnects. No
progressive loss of current free memory or of the largest block was observed.
The new low-water instrumentation attributed the largest initial drop to
gateway/TLS activity while the loop yielded; it did not demonstrate a leak.

**Gateway during the retest.** Wi-Fi connected normally; the pinned CA and host
name were verified; TLS and WebSocket upgrades completed. **Four** gateway
sessions were established without rebooting the robot, with automatic
disconnect, backoff and reconnection. `tlsAllocFail=0`,
`stray=0 unk=0 proto=0 stale=0`, `evDrop=0 txDrop=0`.

**Scope of the run.** The shortened procedure asked for gateway sessions up to
#6. The run **stopped at session #4 by user decision**, because memory had stayed
stable with a large margin. Sessions #5 and #6 were **not** tested.

**Forced-disconnect diagnostics.** While the gateway was deliberately stopped,
the ESP32 networking library printed transient `Bad file number`,
`Connection reset by peer` and `UNKNOWN ERROR CODE` messages. These are
expected upstream diagnostics from a forced disconnect, not failed
reconnections: the firmware recovered automatically each time.

**Capture and playback.**
- A long turn: `duration=24195 ms`, `samples=386560`; the streamer received
  exactly the committed sample count, `gaps=0`, `busyRetries=0`, microphone
  queue stalls 0, capture `failed=0 dropped=0`; playback `queued=403
  played=403`, `samples=386560`, `underruns=0 rejects=0 refusals=0`; audio
  returned to `none`.
- A second turn of about 17.8 s also completed without capture, stream or
  playback errors.

**Rendering-time tradeoff (measured).** `maxTrans` ≈ 24.6 – 27.3 ms;
`maxLoopSpeaking` reached ≈ 29.1 ms; face-render calls occasionally exceeded
20 ms, and the steady-state warning appeared on some face-render frames. This
is the PSRAM rendering cost. It caused no audio underruns, no capture loss, no
network failure and no visible corruption. `TTH_LOOP_WARN_MICROS` stays at
20 ms and the measurements stay visible.

**Diagnostic face cycling.** The `a` key enables the face override, so
heartbeats during the cycle show mismatches such as
`state=ready face=sleeping`. These are expected and are not
connectivity-state defects.

**Decision.** The deterministic PSRAM placement is accepted, and the Step 6.2
memory fix is physically validated. The thresholds and the compile-time
placement are unchanged.

---

### 7.3 Step 6.3 memory points (M3, M4)

The sprites stay in PSRAM (§7.2). During gateway turns the robot logs
`[mem] M3/M4 <point>: internal free=… largest=… (historical min=…) | ring=…B
credit left=… zeroStalls=… | tlsAllocFail=…` at:

| Gate | Point |
|---|---|
| M3 | `capture/upstream` (LISTENING entered), `waiting` |
| M4 | `downstream buffering` (`speech_start`), `playback` (first audio at the speaker), `zero-credit stall` (≤ 1 per 2 s), `back to ready` |

Pass at every point: current internal free ≥ 32 000 B, largest ≥ 24 000 B,
`tlsAllocFail = 0`, no reboot, and no progressive loss across a soak
(`back to ready` free, first vs. last of ten turns). The historical minimum
is recorded separately and is not the gate.

Step 6.3's own internal-RAM cost is small by construction: the ring and the
outbound queue are in PSRAM; `.bss` grows by the source's 1 920 B scratch,
while production drops the mock and its 1 920 B heap scratch.

## 8. State machines

Wi-Fi link and gateway session as in the first revision (backoff 1…30 s and
2…60 s with ±20 % jitter; no retry mid-turn; AuthRejected retries every
5 min only; ProtocolMismatch waits for a reboot). **Added:**

- **Proactive reconnect (D1):** at connection age ≥ 55 min the device closes
  and reconnects **as soon as it is between turns**. It never starts a new
  turn after 58 min without reconnecting first. Worst case, a 46 s capture
  plus a ≤ 120 s response still ends before Cloud Run's 60 min.
- Turn timeouts: first audio 10 s after `turn_end`; mid-response stall 5 s;
  total response 120 s; each → `cancel(N)` + `Error`.

**As implemented in Step 6.2** (`lib/tth_core/…/GatewaySession`, host-tested):

| Situation | Session | Face |
|---|---|---|
| Wi-Fi down, unprovisioned or no CA | Idle | Sleeping |
| TCP + TLS + upgrade, then hello → ready (25 s) | Connecting / AwaitingReady | Sleeping |
| ready received (`out` must be `s16le/24000/1`) | Ready: ping every 15 s, pong within 10 s | Ready |
| failure, timeout, peer close, `error{retry}`, `session_end{replaced}` | Backoff 2, 4, 8, 16, 32, 60 s ±20 % | Sleeping |
| HTTP 401/403 | AuthRejected: retry every 300 s | ERROR (persistent) |
| other 4xx, bad 101, wrong `out`, `error{protocol}` | ProtocolMismatch: no retry | ERROR (persistent) |
| age ≥ 55 min or `session_end{max_age}`, conversation idle | close → reconnect at once | Sleeping briefly |

Losing Wi-Fi closes the session; regaining it reconnects immediately. A new
configuration or CA restarts the session and clears AuthRejected /
ProtocolMismatch. A press is refused (two pulses) unless Wi-Fi is up and the
session is Ready, and at session age ≥ 58 min or after `max_age`. The
conversation's `Connectivity::Failed` puts an idle machine into ERROR, left
only when the link recovers. A connect watchdog (45 s) and a close watchdog
(5 s) recover from a missing transport report.

---

## 9. Faces (D4)

| Situation | Face | Recovery |
|---|---|---|
| booting / unprovisioned / connecting / link lost | **Sleeping** | → Ready when the session is ready |
| online, idle | Ready | — |
| PTT while not online | Sleeping, **no LISTENING**, two short haptic pulses | — |
| transient error (timeout, drop mid-turn, server error, credit violation) | ERROR for **≥ 3 s** | → Ready if online, else Sleeping |
| **AuthRejected**, **ProtocolMismatch** | ERROR, **persistent** | re-provision / reboot |

---

## 10. Barge-in across the network

The Phase 5 sequence is unchanged; step 3 uses §3.2 (purge, priority cancel,
`cancel(N)` before `turn_start(N+1)`). Late frames for N are dropped and
counted **and their credit is returned**.

---

## 11. Tests

### 11.1 Step 6.0 (this step)

**Portable C++ (native):**
- `test_gateway_protocol`: header round trip; **uint32 turn ids** incl.
  0xFFFFFFFF; turn id 0 rejected; flags/reserved non-zero, odd, empty,
  oversize and unknown-kind frames rejected; every control message encoded
  within 512 B; the parser rejects oversized, malformed and wrong-`proto`
  frames and ignores (counts) unknown `t`.
- `test_turn_id_allocator`: starts at 1; skips 0 at wrap; never returns the
  active id or one of the last 16; wraparound near 0xFFFFFFFF with active and
  recently cancelled ids.
- `test_outbound_queue`: **the wire order `turn_start · audio… · turn_end` is
  preserved under a delayed sender, a partially full queue and failed sends**;
  `turn_end` can never overtake audio; an item is removed only after a
  successful send; a failed send clears the queue; priority items only at
  frame boundaries; cancel purges unsent items of N, is dropped if
  `turn_start(N)` was never sent, and always precedes `turn_start(N+1)`;
  reserved capacity for `turn_start`/`turn_end`; `Busy` when full (lossless).
- `test_downstream_credit`: zero credit; partial credit; replenishment
  batching; cancellation with outstanding credit (stale frames return
  credit); a peer exceeding credit → violation counted, frame dropped, no
  blocking; the conservation invariant.

**Gateway (`deno test`):** the protocol codec (the same vectors as C++);
credit (never sends beyond credit; zero, partial and replenished credit;
over-return → close); **ordered downstream lane** (`turn_complete` never
before audio; priority messages at zero credit); turn ids (0 / active /
recent → `bad_turn`); upstream order and `turn_end` count verification;
registry auth (constant time, unknown, disabled, replaced session); rate
limits; limits (§4.6); log redaction; prompt generation and parity; setup
JSON == Flutter's PTT setup; a **fake Gemini server**: full turn, cancel with
outstanding credit, stale suppression, idle close and reopen, reconnect
between turns only, `free_conversation` → PTT conversion log.

**Token function (`deno test`):** missing, malformed, wrong signature,
expired, future, replayed nonce, rate limited, Gemini failure, valid → token.

**Real Gemini (manual, env-gated):** two probes (§5.6) — the `clientContent`
diagnostic (INTERRUPTS, 70 ms, 280 ms old audio) and the production
manual-activity sequence (INTERRUPTS, 62 ms, 240 ms old audio, turn 2
answered). **Fence replay** (`tests/fence_replay_test.ts`): the production
timing through the real `DeviceSession`, both event orders, exact credit.
**Probe attribution** (`tests/probe_attribution_test.ts`): the recorded
production run through the corrected attribution.

**Not runnable on this machine:** the SQL migrations (no Docker, so no local
Postgres). They are delivered with a manual verification script to run in the
Supabase SQL editor.

### 11.2 Later steps

Device and physical tests P1–P12 as in the first revision, plus:
- P13: proactive reconnect at ~55 min, between turns (a shortened timer in
  the diagnostic build);

Step 6.2's gates, as exercised (procedure and expected Serial output in the
firmware README, "Gateway session (Step 6.2)"):
- P3: TLS session to the LAN gateway (pinned dev CA, `GEMINI_MODE=fake`):
  Sleeping → READY on boot; keepalive; gateway stopped → Sleeping, 2…60 s
  backoff, presses refused → restarted → READY without a reboot; wrong token
  → persistent ERROR (AuthRejected, 300 s retries) → fixed → READY; wrong CA
  → TLS failure, no connection; wrong path → ProtocolMismatch; Wi-Fi off/on;
  local mock turns still work while READY.
- P13 (diagnostic build, 2 min / 3 min): reconnect only between turns; press
  refused at the guard.
- M2: `[mem] M2 gateway ready` and every later `gateway ready #N` line over
  ≥ 20 reconnects, against the §7 thresholds, with `tlsAllocFail=0`.
- P14: credit — `creditViolations = 0` over a soak; a zero-credit stall
  recovers.

Step 6.3's gates (procedure and expected Serial output in the firmware
README, "Gateway turns (Step 6.3)"; all with `GEMINI_MODE=fake`):
- P5: a gateway turn end to end with the echo — robot and gateway agree on
  up frames/bytes and response frames/bytes; five turns; barge-in during
  gateway speech; the first-response timeout (`FAKE_FIRST_RESPONSE_DELAY_MS
  = 11000`) fails the turn once and the connection survives; session loss
  while speaking fails the turn once and recovers.
- P7: a response ≥ 4× the ring — the gateway stops exactly at zero credit
  (`zeroStalls` rises, `creditViolations = 0`), fills the credit faster than
  real time (ring high ≥ 193 200 B), resumes after consumption, completes
  with matching bytes, no loss, duplication, overflow, or underrun after
  priming; accounting restored (`left = 192000`).
- P14: a valid soak with `creditViolations = 0`; a forced zero-credit hold
  observable in both logs while pongs still arrive; an intentional over-credit
  frame refused → turn fails once → reconnect → next answer complete.
- M3/M4: §7.3.

**Step 6.3 result (accelerated v1 path, product-owner decision).**

Physically confirmed (production firmware, `GEMINI_MODE=fake`): production
boot and gateway authentication; Wi-Fi, pinned-CA TLS and WebSocket;
upstream microphone audio through `GatewayTurnSource`; the echo returned as
24 kHz audio with exact 16 → 24 kHz duration conversion; downstream credit
reaching zero and resuming; the ring draining to zero; credit returning to
192 000 B; barge-in stopping playback and starting a new capture; the
first-response timeout; the connection remaining usable after it; a gateway
restart followed by a successful turn; memory above the §7 limits; no credit
violations, bad frames, send failures, capture drops or reboots.

Known accepted issue: occasional playback underruns (one completed response
with `underruns=1`, one cancelled response with `underruns=9`); not every
turn had zero underruns. No blocking functional failure was perceived.
Accepted for v1; post-v1 improvement: a startup prebuffer. No code change.

Skipped by product-owner decision — **not passed**: deterministic session
loss during active playback (P5.5), P7 (long-response physical stress), P14
(physical credit soak and intentional over-credit test). Their logic stays
covered by the automated tests (`test_gateway_turn`, `test_downstream_ring`,
the gateway's `fake_echo_test.ts`).

---

## 12. Steps

| Step | Content | Gate |
|---|---|---|
| **6.0** | protocol (C++ + TS), ordering, credit, turn ids; gateway core with fake-Gemini tests; prompt generation + parity; limits, rate limits, redaction; `gateway-gemini-token` + tests; mint-admission and RLS migrations (written, not applied); the real-Gemini barge-in check | host tests + the manual Gemini check + review |
| **6.1** | device Wi-Fi, provisioning, Sleeping face, M0–M1 — **COMPLETE** (P1, P2 passed on the Core2) | P1, P2 |
| **6.2** | TLS WS client, `hello`/`ready`, session state machine, keepalive, auth failure, proactive reconnect — *implemented; first run: gateway path passed, M2 failed in playback → sprites to PSRAM (§7.1) → PSRAM memory retest PASSED, four sessions (§7.2)* | P3, P13, M2 |
| **6.3** | `GatewayTurnSource`, `OutboundQueue`/`NetSender`, credit, playback — **CLOSED for v1**: core behaviour physically confirmed; P5.5, P7, P14 **skipped by product-owner decision (not passed)**; playback underruns a known accepted issue (§11.2) | P5 (partial), M3–M4 |
| 6.4 | *replaced by the accelerated final-v1 plan (under review): real Gemini through the gateway, token-function deployment, production hosting and provisioning, one real conversation, barge-in and reconnect, release documentation* | minimal physical tests |

Deployment gates: no public Cloud Run deployment before
`gateway-gemini-token` is deployed with its secret **and** the RLS migration
is applied.

---

### 12.1 Accelerated final-v1 path — progress

Replaces step 6.4 (product-owner decision). Nothing is deployed publicly;
`restrict_activity_writes` is not applied.

| Step | Result |
|---|---|
| V1 | Reviewed work committed in logical commits and pushed (unrelated Flutter branding changes excluded). |
| V2 | `GATEWAY_MINT_SECRET` generated and set on Supabase project `gashjedpdvcrzwryjiva` (TTH Bot) without being displayed; copy in the ignored `gateway/.env`. |
| V3 | Migration history repaired for the two pre-existing migrations, then only `20260911190000_gateway_mint_admit.sql` applied; RLS, grants and `ok`/`replay`/`rate_limited` verified; activities unchanged. |
| V4 | `gateway-gemini-token` deployed (v3). Google refused the SDK-style `liveConnectConstraints` REST body (HTTP 400, found with a status-only log); the request is now the documented `uses`/`expireTime`/`newSessionExpireTime`. The gateway connects ephemeral-token sessions to `BidiGenerateContentConstrained`. Signed mint → token → Gemini Live `setup_complete` verified; unsigned, malformed, wrong-signature and replayed assertions → 401. |
| V5 | Per-device activity loaded fresh from Supabase with the publishable key, prompt composed like Flutter, fail-closed before Gemini — **physical LAN test PASSED** (below). |

**V5 physical LAN test (2026-09-16), real Gemini, production firmware
`core2-6.3`, device `tth-core2-01`, activity
`a95ffc7e-1406-4a19-ac3b-6c27d8516b70` ("Conversație liberă", conversation,
converted free-conversation → push-to-talk).**

- READY on the pinned dev CA (TLS + upgrade 6.3 s, `tlsAllocFail=0`); every
  device connection logged `session_hello` with the activity id and reached
  `gemini_ready` through a freshly minted token.
- **Four real turns**, all answered audibly: `ok=4 failed=0`. Up frames/bytes
  and response frames/bytes agree exactly between robot and gateway on every
  turn (e.g. turn 4: 165 frames / 105 472 B up; 456 frames / 872 642 B down).
  First response 1.2–1.5 s after `turn_end` was written.
- Capture: `failed=0 dropped=0` on every turn. Playback: `rejects=0
  refusals=0` on every turn; `underruns` 0, 0, **1**, 0 — the known accepted
  post-v1 prebuffer issue, not audible.
- Credit under real Gemini (faster than real time): `zeroStalls` rose to 332,
  ring high 193 200 of 290 304 B, `creditViolations=0` (capacity 0),
  `badFrames=0 sendFail=0`, stale/cancelled/old-connection bytes 0; after each
  answer `spent = consumed = returned`, `left=192000`.
- Session: `state=ready failures=0 sessions=1`, pings answered (RTT 16–127
  ms), `evDrop=0 txDrop=0`; no reboot during the conversation (session age
  continuous to 189 s; the only power-on reset in the log is the deliberate
  reset before the turns).
- Memory (M3/M4 points, current values): lowest internal free 93 892 B with
  largest block 90 100 B (≥ 32 000 / 24 000); `back to ready` 115 320–118 012
  B, no progressive loss. Historical minimum 83 584 B, recorded separately.
- Redaction, checked mechanically on both logs: no mint secret, publishable
  key, device-token digest, Gemini token or API-key shape, JWT, bearer header,
  assertion, activity or base-prompt text (162 prompt fragments checked),
  title or participant name.

## 13. Files

**New:** `gateway/` (Deno service, tests, `scripts/generate_base_prompt.ts`,
`src/generated/edubot_base_prompt.ts`, `Dockerfile`, README,
`.env.example` with no values); `supabase/functions/gateway-gemini-token/`
(+ tests); `supabase/migrations/…_gateway_mint_admit.sql`;
`supabase/migrations/…_restrict_activity_writes.sql` (not applied);
`firmware/core2/lib/tth_core/…`: `GatewayProtocol`, `TurnIdAllocator`,
`OutboundQueue`, `DownstreamCredit` (+ tests). Later steps: the §6 device
components, `tools/provision.py`.

**Modified:** `supabase/config.toml` (`[functions.gateway-gemini-token]`),
`.gitignore`, READMEs. **Unchanged:** the existing `gemini-token` function
and the Flutter app. The web sign-in that the RLS migration needs is a
**separately reviewed Flutter change**.

---

## 14. Risks

1. Barge-in on the production path is measured to interrupt (62 ms, §5.6.2),
   from one run with synthesised input; Gemini's behaviour may change between
   model versions. The fence (5 s timeout) and the closing-`turnComplete`
   rule hold regardless; the bounded fallback stays a documented contingency.
2. Internal RAM under TLS → §7 rule, with the PSRAM fallback.
3. **Certificate dates are unchecked** → a real residual gap. Pinned CA +
   hostname verification + revocable tokens + a rotation path limit the
   exposure; certificate lifetime does not.
4. The RLS migration disables web editing until the web app signs in → a
   Flutter change to review; applying the migration first is fail-safe.
5. The existing `gemini-token` stays unauthenticated for Flutter → a
   pre-existing risk, unchanged; the gateway never uses it. Option A is the
   path to closing it.
6. Cloud Run's 60-min request cap → the 55-min proactive reconnect.
7. SQL is untested locally → manual verification script; a local Postgres
   (Docker) would allow automated tests.

## 15. Out of scope for Phase 6

Full duplex, OTA, an on-device activity picker, `sessionResumption`, Flutter
changes (including web sign-in: a separate review), flash encryption, DSP
for the USB power-path whine, and phone-based Wi-Fi provisioning (BLE, SoftAP,
captive portal or Flutter) — v1 uses USB serial (§4.1); phone provisioning is
a possible post-v1 enhancement.
