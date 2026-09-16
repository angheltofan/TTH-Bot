# TTH Bot — M5Stack Core2 firmware

## Status: PHASE 5 COMPLETE — playback, streaming, haptics, barge-in (offline)

**Phase 5 is complete**, validated on the physical M5Stack Core2: loopback
and synthetic 24 kHz playback, backpressure (`received == committed`,
`gaps=0`), `underruns=0 rejects=0 refusals=0`, barge-in, haptics and face
animation, stable memory, and acceptable loudness at master volume 255. See
[docs/PHASE5_PLAN.md](docs/PHASE5_PLAN.md), and "Power supply and audible
whine" below for the one hardware observation.

Phase 4 is closed with it: its one-off ~32 ms first-frame priming at PTT
start is an accepted, documented transition cost, and its capture path is
the one every Phase 5 turn exercised.

**Phase 6:** Step 6.0 (gateway, protocol, token path) is approved and
complete. **Step 6.1 — USB-serial provisioning, NVS configuration, the Wi-Fi
link and the Sleeping face — is COMPLETE**: physical gates P1 and P2 passed on
the Core2 (stored configuration loaded, Wi-Fi connected, READY → Sleeping on
link loss, offline presses refused with the two-pulse pattern and no capture,
backoff and automatic reconnect without a reboot, Sleeping → READY, stable
memory, `logDrops=0 criticalDrops=0`).

**Step 6.2 — the TLS WebSocket session to the gateway (hello/ready,
keepalive, backoff, auth rejection, proactive reconnect) — is implemented.**
On the Core2 the gateway path, keepalive and local turns work. The first
memory gate failed during playback; after moving the face sprites to PSRAM
the **memory retest PASSED**, with four gateway sessions (see "Memory gate"
below).

**Step 6.3 — gateway turns (speech up, model audio down with credit, the
10 s first-response timeout, barge-in and session loss) — is CLOSED on the
accelerated v1 path**: its core behaviour was confirmed physically with the
LAN fake gateway; the long-response stress (P7), the credit soak and
over-credit test (P14) and deterministic session loss during playback were
**skipped by product-owner decision** (not passed), and occasional playback
underruns are an accepted known issue. See "Gateway turns (Step 6.3)" below.
Production firmware always talks to the gateway; the local mock exists only in
a diagnostic build.

Nothing in the Flutter app (`../../lib`) or the Supabase project
(`../../supabase`) has been modified at any point.

**Present:** M5Unified initialisation, the startup diagnostic report, the
`AudioBus` ownership boundary, a cooperative non-blocking main loop, the
push-to-talk input abstraction with both implementations, the conversation
state machine, the robot face, microphone capture into a preallocated PSRAM
turn buffer, live streaming of the user turn to the gateway turn source
(the offline mock — 16 kHz loopback / 24 kHz synthetic — in diagnostic builds
only), the ordered outbound queue and the credit-controlled downstream ring,
non-blocking native-rate playback, real-amplitude cheek bars, the two-pulse
haptic pattern for refused actions (no vibration when speaking), barge-in, USB-serial provisioning into NVS, the Wi-Fi station
link with backoff, the Sleeping face, the TLS WebSocket gateway session with a
pinned CA, and 547 host-side unit tests.

**Absent by design:** network barge-in against real Gemini, the remaining
turn/inactivity/reconnect timeouts and the soak (Step 6.4); Gemini and
Supabase are never contacted by the robot. No credential or certificate of any
kind is compiled into the firmware.

---

## Provisioning and Wi-Fi (Step 6.1)

The robot holds five values, all entered over USB serial and stored only in
the NVS namespace `tth`: Wi-Fi SSID, Wi-Fi password, gateway URL (`wss://`
only), device id, device token. None is in the firmware image or in Git, and
neither the robot nor the tool ever prints the SSID, password or token —
reports say only *configured / absent* plus safe metadata (a length; the
gateway host, port and path; the device id). Not encrypted at rest (plan D8).

Design details (A/B transactional record, validation rules, reset scope):
[docs/PHASE6_PLAN.md §4.1](docs/PHASE6_PLAN.md).

### Provision a robot

1. Flash the firmware, then **close the serial monitor** (only one program can
   hold the port).
2. Run the tool with the PlatformIO Python (it already has `pyserial`):

   ```powershell
   cd d:\tth-bot\tth_bot\firmware\core2
   & $pio device list                                   # find the COM port
   & "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" tools\provision.py --port COM3
   ```

   It prompts for the SSID, the password (hidden, entered twice; empty = open
   network, which it asks you to confirm), the gateway URL, the device id and
   the device token (hidden; **leave empty to generate one**). Values can come
   from `TTH_WIFI_SSID`, `TTH_WIFI_PASSWORD`, `TTH_GATEWAY_URL`,
   `TTH_DEVICE_ID`, `TTH_DEVICE_TOKEN` instead. Everything is validated before
   anything is sent; secrets travel hex-encoded.
3. Expected tool output:

   ```text
   robot answered; sending configuration (values are not shown)
   [prov] COMMITTED generation 1 to slot A; applying
   [config] stored ssid: configured (12 bytes)
   [config] stored pass: configured
   [config] stored url: configured (wss host=gw.example.test port=443 path=/v1/ws)
   [config] stored id: configured (core2-01)
   [config] stored token: configured (43 chars)
   [config] stored complete: yes
   [config] stored generation 1 in slot A
   [net] wifi=connecting failures=0 lastRetryDelay=0ms rssi=0 connects=0 | config=stored generation=1 slot=A

   Gateway registry entry for this robot (the token itself is not shown):
   { "core2-01": { "token_sha256": "<64 hex>", "activity_id": null, "enabled": true } }
   ```

   The registry entry is for Step 6.2+; nothing is deployed now.
4. The new configuration applies at once — no reboot. Open the monitor to
   watch Wi-Fi come up.

Manual alternative (for example to inspect): in `pio device monitor`, a line
starting with `!` is a command — `!prov help`, `!prov show`, `!prov begin`,
`!prov set <ssid|pass|url|id|token> <value>`, `!prov sethex <field> <hex>`,
`!prov commit`, `!prov abort`. Prefer the tool for secrets. Single keys
(`r l w s e o a b m p x g ?`) work as before; `o` holds the Sleeping face.

### Expected serial output

Boot, not provisioned:

```text
[mem] M0 = the memory report above (all buffers allocated, Wi-Fi not started)
[config] slot A: absent (generation 0) | slot B: absent (generation 0)
[config] no configuration stored: provision over USB serial (!prov help)
[config] stored: none
[app] ready - cooperative loop running
[wifi] not provisioned: Wi-Fi off (provision over USB serial - !prov help)
[app] state booting -> disconnected (face: sleeping)
```

Boot, provisioned, access point reachable:

```text
[config] slot A: valid (generation 1) | slot B: absent (generation 0)
[config] using slot A, generation 1
[config] stored ssid: configured (12 bytes)
[config] stored pass: configured
[config] stored url: configured (wss host=gw.example.test port=443 path=/v1/ws)
[config] stored id: configured (core2-01)
[config] stored token: configured (43 chars)
[config] stored complete: yes
[app] ready - cooperative loop running
[wifi] configuration applied (ssid 12 bytes, password set)
[wifi] connecting (attempt 1)
[app] state booting -> connecting (face: sleeping)
[wifi] connected in 2140 ms: rssi=-54 dBm channel=6 ip=192.168.1.23
[mem] M1 wifi connected: internal free=... largest=... min=... | psram free=... largest=...
[app] state connecting -> ready (face: ready)
```

Every heartbeat now also carries
`[net] wifi=connected failures=0 lastRetryDelay=0ms rssi=-54 connects=1 | config=stored generation=1 slot=A`.

Access point switched off, then on again (gate P2 — no reboot):

```text
[wifi] link lost after 95 s (connection lost) -> retry in 1043 ms
[app] state ready -> disconnected (face: sleeping)
[wifi] connecting (attempt 2)
[app] state disconnected -> connecting (face: sleeping)
[wifi] no connection after 15000 ms (network not found) -> retry in 1874 ms (failures 2)
[app] state connecting -> disconnected (face: sleeping)
[wifi] connecting (attempt 3)
[app] state disconnected -> connecting (face: sleeping)
[wifi] no connection after 15000 ms (network not found) -> retry in 4411 ms (failures 3)
[app] state connecting -> disconnected (face: sleeping)
...                                  retry delays ~1, 2, 4, 8, 16, then 30 s (±20 %)
[wifi] connected in 1830 ms: rssi=-55 dBm channel=6 ip=192.168.1.23
[app] state connecting -> ready (face: ready)
```

The text in parentheses is the Wi-Fi driver's status at that moment
(`connection lost`, `disconnected`, `network not found`, `connect failed`) and
varies with the access point; the delays are jittered, so the exact numbers
differ on every run.

Push-to-talk while not online: no LISTENING, two short pulses.

```text
[ptt] PRESS
[ptt] press refused: not online (wifi backoff)
[haptics] refused pattern (2 short pulses)
```

### Recovery and reset

- **Wrong or changed Wi-Fi password / network:** the robot stays Sleeping and
  logs `no connection after 15000 ms (...)` with growing retry delays. Run
  the tool again with the right values; the commit applies immediately.
- **A command was wrong or provisioning was interrupted** (typo, cable pulled,
  tool stopped): nothing changes until a successful `commit`. A malformed
  command, an invalid value, an incomplete commit or a failed write all leave
  the stored configuration exactly as it was. A write interrupted by power
  loss damages only the non-live slot; the boot line then shows, for example,
  `slot B: checksum mismatch`, and the last valid slot is used.
- **`ERROR commit: busy`:** a turn or playback is active. Wait until the face
  is idle and commit again — the staged values are kept.
- **Erase the TTH Bot configuration** (Wi-Fi, gateway, identity; namespace
  `tth` only — nothing else in NVS):

  ```powershell
  & "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" tools\provision.py --port COM3 --reset
  ```

  The tool asks you to type `ERASE`. Manually: `!prov reset`, then within
  30 s `!prov reset confirm <code>` with the four-digit code it printed. A
  wrong or expired code cancels. Afterwards: `RESET done`, Wi-Fi off,
  `state ... -> disconnected (face: sleeping)`.
- **Check what is stored** at any time: `provision.py --port COM3 --show`, or
  `!prov show` in the monitor.
- **A lost or replaced robot:** reset or reprovision it with a new token and
  remove its entry from the gateway registry (the token is revocable there).

### Changing Wi-Fi later — the v1 method

USB serial through `tools/provision.py` is **the supported provisioning method
for v1**. The Wi-Fi credentials, gateway URL, device identity and CA are data
in NVS, so changing them **never requires reflashing**; firmware is uploaded
only when the firmware code changes.

1. Connect the Core2 to a laptop with USB and close any serial monitor.
2. Change only the Wi-Fi network and password — the device token and its
   gateway registry entry stay valid:

   ```powershell
   cd d:\tth-bot\tth_bot
   & "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" firmware\core2\tools\provision.py --port COM3 --set-wifi
   ```

   Enter the network name and the password (hidden, twice). Expected:
   `robot answered; sending the change (values are not shown)`,
   `[prov] COMMITTED generation N to slot A|B; applying`, then the redacted
   report with `stored complete: yes`. The robot reconnects at once.
3. If the command is refused or interrupted, the previous configuration stays
   in use (transactional NVS) — run it again. The same pattern changes only
   the gateway URL (`--set-url <url>`) or only the CA (`--ca-only <pem>`).

Phone-based provisioning (BLE, SoftAP, a captive portal or the Flutter app) is
not part of v1 and is not implemented in Phase 6; it is a possible post-v1
enhancement.

---

## Gateway session (Step 6.2)

The robot keeps a TLS WebSocket session to the TTH gateway: `hello` →
`ready`, a ping every 15 s, backoff on failure, a persistent ERROR on auth
rejection or protocol mismatch, and a proactive reconnect before Cloud Run's
60-minute limit. *As tested in Step 6.2, turns were still the local mock.
Since Step 6.3 every turn goes through the gateway — see "Gateway turns
(Step 6.3)" below; Stage 9's P3.2 is superseded by P5.*

- One FreeRTOS task on core 0 owns DNS, TCP, TLS, the upgrade and all socket
  I/O; the cooperative loop only exchanges queued events with it.
- TLS verifies the gateway against the **provisioned CA** and the **host
  name**. No CA provisioned → no connection; there is no insecure mode.
  Certificate dates are not checked by this mbedTLS build (PHASE6_PLAN §4.2).
- The device token travels only in the upgrade request, whose buffer is
  zeroed right after sending. Nothing secret is logged.
- Every boot prints `[gw] timings: production build - …` or
  `[gw] timings: DIAGNOSTIC build - …`, so the flashed timing values are
  always visible.

| Serial state | Face | Press |
|---|---|---|
| `wifi` not connected, or `[gw] disabled` | Sleeping | refused, 2 pulses |
| `[gw] connecting` / `hello sent` / backoff | Sleeping | refused, 2 pulses |
| `[gw] READY` | Ready | gateway turn (Step 6.3; a local mock turn in Step 6.2) |
| `*** AUTH REJECTED` / `*** PROTOCOL MISMATCH` | ERROR (dim), persistent | refused, 2 pulses |

### Physical test procedure — P3, P13, M2

Nothing is deployed: the gateway runs on this PC with a simulated Gemini
(`GEMINI_MODE=fake`). The robot is on **COM3**.

**Windows used.** *Window A*: a normal PowerShell for the robot (builds,
uploads, `provision.py`, the serial monitor). *Window B*: a normal PowerShell
for the gateway. *Window C*: PowerShell **Run as administrator**, only for
the firewall rule. Only one program can use COM3 at a time: **leave the serial
monitor with Ctrl+C before every `provision.py` or upload command**, and open
it again afterwards.

**Secrets.** No command below contains a secret or a value to edit: the Wi-Fi
password is typed only at hidden prompts, the device token is generated on the
robot and never shown, and `gateway/.env` and `gateway/.dev-certs/` are
ignored by Git. The token digest (`token_sha256`) is not a secret, but paste it
only at the `make_dev_env.ps1` prompt — never into a tracked file.

**Recovery in general.** Uploading firmware never erases the stored
configuration. Every `provision.py` change is a transactional commit: a
refused, interrupted or failed command leaves the last valid configuration in
use, and a wrong value is fixed by running the command again with the right
value (`--set-url`, `--set-wifi`, `--ca-only`). `--reset` (full erase) is the
last resort and is not needed anywhere below.

#### Stage 0 — session variables (Window A)

```powershell
cd d:\tth-bot\tth_bot
$env:PLATFORMIO_CORE_DIR = "D:\pio-core"
$pio  = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
$py   = "$env:USERPROFILE\.platformio\penv\Scripts\python.exe"
$bash = "C:\Program Files\Git\bin\bash.exe"
Test-Path Env:PLATFORMIO_BUILD_FLAGS
```

- **Expected:** `False`.
- **Fail:** `True` — a diagnostic flag is still set: `Remove-Item Env:PLATFORMIO_BUILD_FLAGS`, then repeat.

Repeat Stage 0 in any new Window A.

#### Stage 1 — production firmware on the robot (Window A)

```powershell
& $pio run -d firmware\core2 -e core2-touch -t upload --upload-port COM3
& $pio device monitor --port COM3 --baud 115200
```

- **Manual:** when the monitor is open, press the Core2's reset button (or
  unplug and replug the USB cable).
- **Expected:** the upload ends with `[SUCCESS]`; the boot report contains
  `TTH Bot - M5Stack Core2 - PHASE 6.2 gateway session (TLS WebSocket; turns still local)`
  and
  `[gw] timings: production build - proactive reconnect 3300 s, turn guard 3480 s, ping 15 s, pong 10 s, auth retry 300 s`.
  Without a CA yet: `[config] stored ca: absent (…)` and `[gw] disabled: no CA provisioned (…)`.
- **Pass:** both lines present. **Fail:** `DIAGNOSTIC build` → Stage 12.
- **Recovery:** `could not open port 'COM3'` → close every monitor or serial
  program; `& $pio device list` must show COM3. Leave the monitor with Ctrl+C.

#### Stage 2 — this PC's LAN IPv4 address (Window A)

```powershell
Get-NetIPConfiguration | Where-Object { $_.IPv4DefaultGateway -and $_.NetAdapter.Status -eq "Up" } | Format-Table InterfaceAlias, @{n="IPv4";e={$_.IPv4Address.IPAddress}}, @{n="Router";e={$_.IPv4DefaultGateway.NextHop}}
Get-NetConnectionProfile | Format-Table InterfaceAlias, Name, NetworkCategory
$ip = (Read-Host "This PC's LAN IPv4 address").Trim()
if ($ip -match '^(25[0-5]|2[0-4]\d|1?\d?\d)(\.(25[0-5]|2[0-4]\d|1?\d?\d)){3}$') { "gateway URL: wss://${ip}:8443/v1/ws" } else { "NOT an IPv4 address - run the Read-Host line again" }
```

- **Manual:** at the prompt, type the IPv4 of the adapter connected to the
  **same network the robot's Wi-Fi joins** (usually `Wi-Fi` or `Ethernet`;
  not `vEthernet`, VPN or VirtualBox adapters; never `169.254.…`).
- **Expected:** `gateway URL: wss://<that address>:8443/v1/ws`, and that
  adapter's `NetworkCategory` is `Private`.
- **Fail:** `Public` → Windows Settings → Network & internet → the network →
  *Private network*; then repeat Stage 2.
- **Recovery:** if the PC's address changes later (DHCP), repeat Stage 3 (the
  CA is reused) and `--set-url` (P3.6 restore command), then restart the gateway.

`$ip` exists only in this window; set it again (the `Read-Host` line) in a new Window A.

#### Stage 3 — LAN development CA and gateway certificate (Window A)

```powershell
& $bash gateway/scripts/make_dev_certs.sh $ip
git check-ignore -v gateway/.dev-certs/ca.key gateway/.dev-certs/gateway.key
git status --porcelain --ignored gateway/.dev-certs
```

- **Expected:** `Certificate request self-signature ok`, `subject=CN=<address>`,
  `written to .dev-certs`, `names: IP:<address>,DNS:<address>`; then two
  `.gitignore:…:gateway/.dev-certs/` lines and `!! gateway/.dev-certs/`.
- **Pass:** the names line shows this PC's address, and Git reports the folder
  as ignored (`!!`). **Fail:** `openssl` errors (install Git for Windows) or any
  line without `!!`.
- **Recovery:** re-running is safe: the existing CA is reused, so a robot that
  already holds `ca.pem` needs nothing new. Deleting `gateway\.dev-certs`
  creates a new CA, which must then be provisioned again (`--ca-only`).

#### Stage 4 — provision the robot: Wi-Fi, URL, device id, new token, CA (Window A)

Leave the monitor (Ctrl+C) first.

```powershell
$env:TTH_GATEWAY_URL = "wss://${ip}:8443/v1/ws"
$env:TTH_DEVICE_ID = "core2-01"
Remove-Item Env:TTH_DEVICE_TOKEN -ErrorAction SilentlyContinue
& $py firmware\core2\tools\provision.py --port COM3 --ca gateway\.dev-certs\ca.pem
Remove-Item Env:TTH_GATEWAY_URL, Env:TTH_DEVICE_ID
```

- **Manual:** the Wi-Fi network name; the Wi-Fi password twice (hidden); at
  `Device token (empty = generate a new one):` press **Enter**. Then select
  the 64 hexadecimal characters after `"token_sha256":` and copy them
  (select, then Enter).
- **Expected:**

  ```text
  Gateway URL (wss://...): (from TTH_GATEWAY_URL)
  Device id (1-32 of A-Z a-z 0-9 _ -): (from TTH_DEVICE_ID)
  robot answered; sending configuration (values are not shown)
  [prov] COMMITTED generation N to slot A|B; applying
  [prov] CA COMMITTED generation 1 to slot A (1 certificate(s), … bytes); applying
  [config] stored ssid: configured (… bytes)
  [config] stored pass: configured
  [config] stored url: configured (wss host=<address> port=8443 path=/v1/ws)
  [config] stored id: configured (core2-01)
  [config] stored token: configured (43 chars)
  [config] stored complete: yes
  [config] stored ca: configured (1 certificate(s), … bytes, generation 1, slot A): 1 certificate(s), first subject CN=TTH Bot LAN Dev CA

  Gateway registry entry for this robot (the token itself is not shown):
  { "core2-01": { "token_sha256": "…64 hex…", "activity_id": null, "enabled": true } }
  ```

  (The JSON is printed over several lines; `CA COMMITTED` shows a higher
  generation if a CA was stored before.)
- **Pass:** both `COMMITTED` lines, `complete: yes`, `ca: configured`, a
  64-character digest. **Fail:** `refused by the robot: …` or `commit failed`.
- **Recovery:** a refusal names the field and rule and stores nothing of that
  part; the previous configuration stays in use — run the command again.
  `did not answer '!prov ping'` → close the monitor, check COM3. If the digest
  was lost, run Stage 4 again (a new token and digest).

#### Stage 5 — `gateway/.env` (Window A)

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File gateway\scripts\make_dev_env.ps1
git status --porcelain --ignored gateway/.env
```

- **Manual:** `Device id provisioned on the robot` → `core2-01`;
  `token_sha256 printed by provision.py` → paste the digest.
- **Expected:** `gateway/.env is ignored by Git`,
  `wrote gateway/.env (device core2-01, GEMINI_MODE=fake, port 8443)`,
  `!! gateway/.env`.
- **Pass:** all three. **Fail:** `NOT ignored by Git` (nothing is written —
  report it), `must be exactly 64 hexadecimal characters`, or a missing `!!`.
- **Recovery:** run the script again; it asks `Type yes to replace it`.

#### Stage 6 — firewall rule (Window C, administrator; once)

```powershell
Get-NetFirewallRule -DisplayName "TTH gateway LAN dev (TCP 8443)" -ErrorAction SilentlyContinue | Format-Table DisplayName, Enabled, Profile, Action
```

If that prints nothing:

```powershell
New-NetFirewallRule -DisplayName "TTH gateway LAN dev (TCP 8443)" -Direction Inbound -Protocol TCP -LocalPort 8443 -Profile Private -Action Allow
```

- **Expected:** a rule with `Enabled True`, `Profile Private`, `Action Allow`.
- **Also:** if Windows asks whether to allow `deno` when the gateway starts,
  allow it on **private** networks only.
- **Recovery:** if the robot later reports `network or timeout` while
  `/healthz` works from the PC, a block rule for deno may exist:
  `Get-NetFirewallApplicationFilter -Program "D:\deno\deno.exe" | Get-NetFirewallRule | Format-Table DisplayName, Action, Profile`
  — disable any `Block` rule it lists in *Windows Defender Firewall with
  Advanced Security*. The rule is removed in Stage 13.

#### Stage 7 — start the LAN gateway (Window B)

```powershell
cd d:\tth-bot\tth_bot\gateway
D:\deno\deno.exe task start:lan
```

- **Expected:** `{"ts":"…","level":"warn","event":"gemini_mode","mode":"fake","code":"ok"}`
  and `{"ts":"…","level":"info","event":"listening","count":8443}`.

Check it from Window A — first which certificate this PC actually receives,
then the endpoint:

```powershell
& $bash -c "openssl s_client -connect ${ip}:8443 -servername $ip </dev/null 2>/dev/null | openssl x509 -noout -issuer"
curl.exe --http1.1 --ssl-no-revoke --cacert gateway\.dev-certs\ca.pem "https://${ip}:8443/healthz"
```

- **Expected:** `issuer=CN=TTH Bot LAN Dev CA`, then `ok`.
- **Pass:** both lines.
- **Antivirus HTTPS scanning (found on this development PC).** If the issuer
  reads `… O=Avast Web/Mail Shield, CN=Avast Web/Mail Shield Untrusted Root`,
  Avast is intercepting HTTPS made by programs on this PC, and `curl` fails with
  `schannel: the certificate chain is incomplete` although the gateway and its
  certificate are correct. Add an Avast exception for this gateway address
  (`https://` + the address + `:8443/*`) in Avast's settings under
  *Exceptions*, or switch off HTTPS scanning for the test, then repeat both
  commands. The interception concerns connections made *from* this PC; the
  robot's connection *to* the gateway is inbound and is not expected to be
  affected — Stage 8 shows it either way.
- **Fail:** the issuer is the dev CA but `curl` reports a certificate error →
  the certificate lacks this address (Stage 3 with the right `$ip`, then
  restart the gateway); `connect` errors or `Failed to connect` → the gateway
  is not running or `$ip` is wrong. (Both checks run on the PC itself, so they
  do not test the firewall.)
- **Recovery:** Ctrl+C stops the gateway; the start command restarts it.

#### Stage 8 — boot to READY (Window A)

```powershell
& $pio device monitor --port COM3 --baud 115200
```

- **Manual:** press the Core2's reset button.
- **Expected (robot):**

  ```text
  [config] ca slot A: valid (generation 1) | slot B: absent (generation 0)
  [config] stored ca: configured (1 certificate(s), … bytes, generation 1, slot A): 1 certificate(s), first subject CN=TTH Bot LAN Dev CA
  [gw] network task on core 0 (priority 2, stack 8192 B); TLS with the pinned CA and host name, no insecure mode
  [gw] timings: production build - proactive reconnect 3300 s, turn guard 3480 s, ping 15 s, pong 10 s, auth retry 300 s
  [app] ready - cooperative loop running
  [wifi] connecting (attempt 1)
  [gw] target wss://<address>:8443/v1/ws as device core2-01 (CA: 1 certificate(s))
  [app] state booting -> connecting (face: sleeping)
  [wifi] connected in … ms: rssi=… dBm channel=… ip=…
  [gw] connecting to wss://<address>:8443/v1/ws (attempt 1)
  [gw] TLS + WebSocket upgrade ok in … ms (CA and host name verified; internal free=… largest=…)
  [gw] hello sent (proto 1, fw core2-6.2, credit 192000 B)
  [gw] READY session=… activity=00000000-0000-4000-8000-000000000000 … ms after connect start (sessions 1)
  [mem] M2 gateway ready #1: internal free=… largest=… min=… | psram free=… largest=… | tlsAllocFail=0
  [net] connectivity connecting -> online
  [app] state connecting -> ready (face: ready)
  ```

- **Expected (gateway):** `connected`, `session_hello` (`"fw":"core2-6.2"`), `gemini_ready`.
- **Pass:** `[gw] READY` and the Ready face within about a minute of the reset. Record the `[mem] M2` line.
- **Fail and recovery** (the robot keeps retrying on its own while you fix the cause):

  | Robot line | Cause | Fix |
  |---|---|---|
  | `[gw] disabled: no CA provisioned` | no CA | `& $py firmware\core2\tools\provision.py --port COM3 --ca-only gateway\.dev-certs\ca.pem` |
  | `network or timeout (detail -1)` or `dns lookup failed` | gateway down, wrong address, firewall | Stages 2, 6, 7 |
  | `tls failure (certificate or protocol)` | certificate lacks this address, or wrong CA | Stage 7 issuer check; Stage 3 with the right `$ip`, restart the gateway; if the issuer check passes and this persists, report it |
  | `http 401 (unauthorized)` → `AUTH REJECTED` | digest in `.env` ≠ robot's token | Stage 5 again, restart the gateway, reset the robot |
  | `http 404` → `PROTOCOL MISMATCH` | wrong URL path | `--set-url` (P3.6 restore command) |
  | `http 503` | gateway not in fake mode | Stage 5 again, restart the gateway |

#### Stage 9 — P3 (production firmware, Windows A and B)

Each step starts from READY and must end in READY again, **without a reboot**
(no new `TTH Bot - M5Stack Core2` banner).

**P3.1 Keepalive.** Wait 2 minutes.
- **Expected:** heartbeat lines
  `[gw] state=ready failures=0 … sessions=1 rtt=…/…ms pings=N stray=0 unk=0 proto=0 stale=0 | tlsAllocFail=0 … evDrop=0 txDrop=0`,
  `pings` rising by about 4 per minute.
- **Pass:** as shown, and no `protocol_error` in Window B.

**P3.2 Local turn while READY.** Hold the centre touch button, speak for a few
seconds, release.
- **Expected:** the Phase 5 sequence (`[app] state ready -> listening`, …,
  `speaking`, `-> ready`) and the loopback playback.
- **Pass:** the turn completes and the next `[gw]` heartbeat still shows `state=ready`.

**P3.3 Gateway stopped and restarted.** Window B: Ctrl+C.
- **Also expected:** while the gateway is down, the ESP32 networking library
  may print transient `Bad file number`, `Connection reset by peer` or
  `UNKNOWN ERROR CODE` lines. These are upstream diagnostics of the forced
  disconnect, not failures; what matters is automatic recovery to READY.
- **Expected (robot):** `[gw] connection closed: connection lost, code 0, after … s`
  (or `closed by the gateway`), `[gw] closed by the gateway -> retry in ~2000 ms (failures 1)`,
  `[net] connectivity online -> offline`, `[app] state ready -> disconnected (face: sleeping)`,
  then `[gw] connect failed after … ms: network or timeout (detail -1)` with retry delays
  of about 4, 8, 16, 32, then 60 s (±20 %).
- **Manual:** press the button while it is Sleeping.
- **Expected:** `[ptt] press refused: not online (wifi connected, gateway backoff)`,
  `[haptics] refused pattern (2 short pulses)`, no LISTENING.
- **Restore:** after at least 2 minutes, Window B: `D:\deno\deno.exe task start:lan`.
- **Pass:** `[gw] READY … (sessions 2)` within about 75 s, face Ready.

**P3.4 Wrong token (AuthRejected).** Window B: Ctrl+C, then:

```powershell
$env:TTH_DEVICES = '{"core2-01":{"token_sha256":"' + ('0' * 64) + '","activity_id":null,"enabled":true}}'
D:\deno\deno.exe task start:lan
```

(A variable set in the shell takes precedence over `.env`; nothing is written to disk.)
- **Expected (robot):** `[gw] connect failed after … ms: http 401 (unauthorized)`,
  `[gw] *** AUTH REJECTED: the gateway refused this device id / token - check its registry entry; ERROR face; retrying every 300 s ***`,
  `[net] connectivity … -> failed`, `[app] state … -> error (face: error)`.
- **Expected (gateway):** `auth_denied` with `"reason":"bad_token"`.
- **Manual:** press the button → `press refused: not online (wifi connected, gateway auth-rejected)` and 2 pulses. Wait 5 minutes: one more `http 401`, still ERROR.
- **Pass:** the ERROR face persists, with retries only every 300 s.
- **Restore:** Window B: Ctrl+C, `Remove-Item Env:TTH_DEVICES`, `D:\deno\deno.exe task start:lan`. READY follows within 300 s (reset the robot for an immediate retry).

**P3.5 Wrong CA (no connection).** Window A (monitor closed):

```powershell
$wrong = Join-Path $env:TEMP "tth-wrong-ca"
$env:TTH_DEV_CERT_DIR = $wrong.Split([char]92) -join "/"
& $bash gateway/scripts/make_dev_certs.sh $ip
Remove-Item Env:TTH_DEV_CERT_DIR
& $py firmware\core2\tools\provision.py --port COM3 --ca-only "$wrong\ca.pem"
& $pio device monitor --port COM3 --baud 115200
```

- **Expected:** `[prov] CA COMMITTED generation 2 …`; then, repeating with backoff:
  `[gw] connect failed after … ms: tls failure (certificate or protocol) (mbedtls -9984, …)`
  (the code may differ). Window B shows no `connected`.
- **Pass:** no `[gw] READY` for 3 minutes. **Fail:** any READY — report it immediately.
- **Restore** (monitor closed):

  ```powershell
  & $py firmware\core2\tools\provision.py --port COM3 --ca-only gateway\.dev-certs\ca.pem
  Remove-Item -Recurse -Force $wrong
  ```

  Then READY at once (`CA COMMITTED generation 3`).

**P3.6 Wrong path (ProtocolMismatch).** Window A (monitor closed):

```powershell
& $py firmware\core2\tools\provision.py --port COM3 --set-url "wss://${ip}:8443/v1/nope"
& $pio device monitor --port COM3 --baud 115200
```

- **Expected:** `[prov] COMMITTED …`; robot: `[gw] connect failed after … ms: http 404 (request rejected)`,
  `[gw] *** PROTOCOL MISMATCH (request rejected): check the gateway URL and firmware; ERROR face; no retry until reprovisioned or rebooted ***`,
  face ERROR.
- **Pass:** no further `[gw] connecting` for 3 minutes.
- **Restore** (monitor closed):

  ```powershell
  & $py firmware\core2\tools\provision.py --port COM3 --set-url "wss://${ip}:8443/v1/ws"
  ```

  READY at once; the token, Wi-Fi and CA are untouched.

**P3.7 Gateway-side failure.** Window B: Ctrl+C, then:

```powershell
$env:FAKE_GEMINI_SETUP = "fail"
D:\deno\deno.exe task start:lan
```

- **Expected (robot):** `[gw] hello sent …`,
  `[gw] gateway error code=gemini_unavailable retry=true`,
  `[gw] connection closed: … code 1011 …` (a `[gw] closing (gateway error message)` line may precede it),
  `[gw] gateway error message -> retry in … ms (failures N)`, repeating with growing delays; face Sleeping.
- **Expected (gateway):** `gemini_failed` with `"code":"gemini_unavailable"`.
- **Pass:** backoff, not ERROR.
- **Restore:** Ctrl+C, `Remove-Item Env:FAKE_GEMINI_SETUP`, `D:\deno\deno.exe task start:lan` → READY.

**P3.8 Wi-Fi off and on.** Switch the access point off.
- **Expected:** `[wifi] link lost …`, `[gw] closing (wi-fi lost)`, `[gw] idle (wi-fi lost)`, face Sleeping.
- Switch it on. **Expected:** `[wifi] connected …`, then at once `[gw] connecting … (attempt 1)` and READY.
- **Pass:** no reboot; READY again.

Record the last `[mem]` and `[gw]` heartbeat lines. P3 passes when P3.1–P3.8 all pass.

#### Stage 10 — P13, proactive reconnect (diagnostic build, Window A)

The diagnostic build reconnects after 2 minutes and refuses turns after 3 minutes. Close the monitor, then:

```powershell
$env:PLATFORMIO_BUILD_FLAGS = "-DTTH_DIAGNOSTIC_BUILD=1"
& $pio run -d firmware\core2 -e core2-touch -t clean
& $pio run -d firmware\core2 -e core2-touch -t upload --upload-port COM3
& $pio device monitor --port COM3 --baud 115200
```

- **Expected:** `[SUCCESS]`; boot:
  `[gw] timings: DIAGNOSTIC build - proactive reconnect 120 s, turn guard 180 s, ping 15 s, pong 10 s, auth retry 300 s`;
  the stored configuration and CA are still there, so READY follows.
- **Fail:** `production build` → `$env:PLATFORMIO_BUILD_FLAGS` was not set in this window; repeat.

**P13.1 Idle.** Do nothing for about 2 minutes after READY.
- **Expected:**

  ```text
  [gw] closing (proactive reconnect)
  [gw] connection closed: closed by the robot, code 1000, after 120 s
  [gw] connecting to wss://<address>:8443/v1/ws (attempt 1)
  [gw] TLS + WebSocket upgrade ok in … ms (…)
  [gw] hello sent (proto 1, fw core2-6.2, credit 192000 B)
  [gw] READY session=… (sessions 2)
  ```

- **Pass:** `attempt 1` (no backoff); Sleeping only for a few seconds.

**P13.2 Never mid-turn.** About 1 min 50 s after a READY, hold the button,
speak for about 10 s and release.
- **Expected:** no `[gw] closing` while listening, waiting or speaking; `[gw] closing (proactive reconnect)` within a second after `-> ready`.
- **Pass:** as described.

**P13.3 The turn guard.** About 1 min 45 s after a READY, hold the button for
the full 45 s (`[ptt] FORCED RELEASE after 45000 ms`) while speaking. While the
loopback plays back (after the 3-minute mark), tap the button.
- **Expected:** `[ptt] press refused: gateway session due for reconnect (age 18x s)` and 2 pulses; the playback continues; after it ends, `[gw] closing (proactive reconnect)` and READY.
- **Pass:** as described.

Keep the diagnostic firmware for Stage 11.

#### Stage 11 — M2, memory over ≥ 20 reconnects (diagnostic build, Window A)

Close the monitor, then:

```powershell
Push-Location $env:TEMP
& $pio device monitor --port COM3 --baud 115200 --filter log2file
```

Leave the robot idle for **at least 45 minutes** (one reconnect every ~2 min),
then Ctrl+C and:

```powershell
Pop-Location
$log = Get-ChildItem $env:TEMP -Filter "platformio-device-monitor-*.log" | Sort-Object LastWriteTime | Select-Object -Last 1
Select-String -Path $log.FullName -Pattern "gateway ready #" | ForEach-Object { $_.Line }
Select-String -Path $log.FullName -Pattern "TTH Bot - M5Stack Core2|Guru Meditation|abort\(\)" | ForEach-Object { $_.Line }
```

- **Expected:** at least 21 lines
  `[mem] -- gateway ready #N: internal free=… largest=… min=… | psram free=… largest=… | tlsAllocFail=0`;
  the second search shows at most one banner (from opening the monitor) and nothing else.
- **Pass** (PHASE6_PLAN §7) — on every line: the current `free=` ≥ 32000,
  the first `largest=` (internal) ≥ 24000, `tlsAllocFail=0`, no reboot.
  `min=` is the historical low-water mark since boot: record it, and report
  any `[mem] new internal low-water` lines, but it is not the gate by itself.
- **Fail:** any value below — report every line; the face-sprite decision is taken from these numbers. No configuration change is involved.

The log holds no secrets. Delete it after recording: `Remove-Item $log.FullName`.

#### Stage 12 — restore the production firmware (Window A)

Close the monitor, then:

```powershell
Remove-Item Env:PLATFORMIO_BUILD_FLAGS -ErrorAction SilentlyContinue
Test-Path Env:PLATFORMIO_BUILD_FLAGS
[Environment]::GetEnvironmentVariable("PLATFORMIO_BUILD_FLAGS", "User")
[Environment]::GetEnvironmentVariable("PLATFORMIO_BUILD_FLAGS", "Machine")
& $pio run -d firmware\core2 -e core2-touch -t clean
& $pio run -d firmware\core2 -e core2-touch -t upload --upload-port COM3
& $pio device monitor --port COM3 --baud 115200
```

- **Expected:** `False`, then two empty lines, `[SUCCESS]`, and on boot
  `[gw] timings: production build - proactive reconnect 3300 s, turn guard 3480 s, ping 15 s, pong 10 s, auth retry 300 s`,
  then READY.
- **Pass:** the `production build` line with 3300 s / 3480 s.
- **Fail:** `DIAGNOSTIC build` → close this window, open a new Window A (Stage 0), and repeat Stage 12.

#### Stage 13 — finish (Windows B, C, A)

- **Window B:** Ctrl+C.
- **Avast:** remove the exception added in Stage 7, if you added one.
- **Window C (administrator):**

  ```powershell
  Remove-NetFirewallRule -DisplayName "TTH gateway LAN dev (TCP 8443)"
  ```

- **Window A:**

  ```powershell
  git status --porcelain --ignored gateway/.env gateway/.dev-certs
  & $bash scripts/check_no_secrets.sh
  ```

- **Expected:** `!! gateway/.dev-certs/`, `!! gateway/.env`, and `no secrets found in … files`.
- **Pass:** both ignored, scan clean.

`gateway\.dev-certs` and `gateway\.env` may stay for Step 6.3 (both ignored).
Deleting `.dev-certs` means the robot needs a new CA later.

**Report back:** the P3.1–P3.8 and P13.1–P13.3 results, the `[mem] M2` line,
all `gateway ready #N` lines from Stage 11, the final `[gw]` heartbeat, and the
Stage 12 timings line.

### Memory gate — first physical run and the sprite decision

First physical Step 6.2 run, sprites in internal DRAM, `tlsAllocFail=0`
throughout:

| Point | internal free | largest block | historical min |
|---|---:|---:|---:|
| M1, after Wi-Fi | 73 496 | 69 620 | 70 080 |
| during TLS / WebSocket connect | 31 356 | 30 708 | 28 444 |
| M2, gateway ready #1 | 36 328 | 28 660 | 26 252 |
| **during playback** | **28 472** | **25 588** | 24 348 |
| after playback, idle | 36 100 | 28 660 | 24 348 |
| later, idle | 36 100 | 28 660 | 12 140 |

- **`free` / `largest` are the heap now; `min` is the lowest `free` since
  boot.** The gate (current free ≥ 32 KB, largest ≥ 24 KB) is judged on the
  current figures at each operating point.
- **Result:** idle at M2 passes, **playback fails** (28 472 < 32 KB; the
  largest block is only ~1.6 KB above 24 KB).
- **The 12 140 B minimum** was a transient of ~12 KB while idle — free and
  largest recovered — and is not shown to be a leak. The old instrumentation
  could not say what caused it; the firmware now logs
  `[mem] new internal low-water …` with the loop stage and the operating state
  whenever the minimum falls ≥ 2 KB, and each gateway connect logs its
  `low-water before->after`.
- **Decision:** both face sprites in PSRAM, fixed at compile time, no
  fallback (see "Sprite placement" in the face section). Expected: about
  81 KB more internal free at every point, slower rendering (full-face
  transitions ~25 ms).

### PSRAM memory retest — PASSED

Production firmware, both sprites confirmed in PSRAM (`sprites=PSRAM` in the
heartbeat); the face stayed functional and visually correct.

| Point | internal free | largest block |
|---|---:|---:|
| M1, after Wi-Fi | ≈ 154 776 | ≈ 110 580 |
| gateway READY | ≈ 117 568 – 117 580 | 94 196 |
| during playback | ≈ 109 728 | 94 196 |
| after playback | ≈ 117 368 | 94 196 |

- **Thresholds:** every current-free and largest-block figure is far above
  32 000 B / 24 000 B. The historical minimum stayed around 85–100 KB in the
  later tests (one value 90 068 B after repeated reconnects). No progressive
  loss of free memory or largest block was seen. The low-water instrumentation
  attributed the largest initial drop to gateway/TLS activity while the loop
  yielded — no leak demonstrated.
- **Gateway:** pinned CA and host name verified; TLS and WebSocket upgrades
  completed. **Four sessions** without a reboot, with automatic disconnect,
  backoff and reconnection. `tlsAllocFail=0`, `stray=0 unk=0 proto=0 stale=0`,
  `evDrop=0 txDrop=0`.
- **Shortened by decision:** the procedure asked for sessions up to #6; the
  run stopped at **session #4** by user decision because memory stayed stable
  with a large margin. Sessions #5 and #6 were not tested.
- **Forced disconnects:** while the gateway was deliberately stopped, the ESP32
  library printed transient `Bad file number`, `Connection reset by peer` and
  `UNKNOWN ERROR CODE` messages — expected upstream diagnostics, not failed
  reconnections; the firmware recovered automatically.
- **Capture and playback:** a 24 195 ms turn (`samples=386560`; the streamer
  received exactly the committed count; `gaps=0 busyRetries=0`; microphone
  queue stalls 0; `failed=0 dropped=0`; playback `queued=403 played=403
  samples=386560 underruns=0 rejects=0 refusals=0`; audio back to `none`), and
  a second turn of about 17.8 s, both without errors.
- **Rendering cost:** `maxTrans` ≈ 24.6–27.3 ms, `maxLoopSpeaking` ≈ 29.1 ms,
  occasional face-render calls over 20 ms and steady-state warnings on some
  face frames — the measured PSRAM cost, with no audio, capture, network or
  visual regression. `TTH_LOOP_WARN_MICROS` unchanged.
- **Face override:** during the `a` cycle, heartbeats show mismatches such as
  `state=ready face=sleeping`; expected, not a connectivity defect.
- **Decision:** deterministic PSRAM placement accepted; the Step 6.2 memory
  fix is physically validated.

### Memory retest after the PSRAM move (shortened) — completed, PASSED

(Kept for reference; results in "PSRAM memory retest — PASSED" above. The run
stopped at gateway session #4 by user decision.)

Only what the placement change can affect is repeated. Everything runs with
the **production** firmware; the LAN gateway from Stages 2–7 is reused
(`gateway\.env` and `gateway\.dev-certs` already exist). Windows A and B as
above.

**R0 — session (Window A).** Run Stage 0. Expected `False`.

**R1 — flash and log to a file (Window A).**

```powershell
& $pio run -d firmware\core2 -e core2-touch -t upload --upload-port COM3
Push-Location $env:TEMP
& $pio device monitor --port COM3 --baud 115200 --filter log2file
```

Window B: `cd d:\tth-bot\tth_bot\gateway` then `D:\deno\deno.exe task start:lan`.
Press the Core2's reset button.

- **Expected boot lines:**
  `[face] eyes sprite 260x116 (60320 bytes) in PSRAM at 0x3f8…`,
  `[face] lower face sprite 226x46 (20792 bytes) in PSRAM at 0x3f8…`,
  `face sprites    : 81112 bytes, eyes in PSRAM, lower face in PSRAM`,
  `[gw] timings: production build - proactive reconnect 3300 s, …`.
- **Pass:** both sprites in PSRAM, face visible. **Fail:**
  `*** … face disabled (no fallback) ***` — stop and report the line.

**R2 — M1 after Wi-Fi.** Record `[mem] M1 wifi connected: internal free=…
largest=… min=…`. Expected free ≈ 150 KB.

**R3 — M2 after gateway readiness.** Record
`[gw] TLS + WebSocket upgrade ok in … (… low-water A->B)` and
`[mem] M2 gateway ready #1: internal free=… largest=… min=…`.
**Pass:** free ≥ 32000 and largest ≥ 24000 (expected ≈ 117 KB free).

**R4 — memory during playback.** Hold the button and speak continuously for
about **15 s**, release, and let the loopback play to the end (≥ 10 s of
speaking guarantees a heartbeat during SPEAKING). Repeat three times. During
the holds the eyes keep blinking, so capture runs alongside the slower frames.

- **Record:** for each turn, the `[mem]` line that follows an
  `[hb] … state=speaking` line; the capture `SUMMARY` lines; the playback
  `SUMMARY` lines; `[pb] … maxLoopSpeaking=…us`.
- **Pass:** on every speaking `[mem]` line, free ≥ 32000 and largest ≥ 24000;
  every `[capture] SUMMARY2` shows `failed=0 dropped=0`; every
  `[play] SUMMARY1` shows `underruns=0 rejects=0 refusals=0`.

**R5 — memory after playback.** Leave it idle for 1 minute; record two `[mem]`
lines. **Pass:** free and largest back near the R3 figures, both above the
thresholds.

**R6 — several reconnects.** In Window B, five times: Ctrl+C, then
`D:\deno\deno.exe task start:lan`, and wait for `[gw] READY` before the next
restart.

- **Record:** each `[gw] TLS + WebSocket upgrade ok … low-water A->B` and
  `[mem] -- gateway ready #N: … | tlsAllocFail=…` (#2 to #6).
- **Pass:** every one has free ≥ 32000, largest ≥ 24000 and `tlsAllocFail=0`,
  and free does not trend downwards across #2–#6.

**R7 — face timing and visual behaviour.** In the monitor, type `a` (cycles
every face, 2.5 s each), watch one full cycle, then press the button to
return to the real face; wait at least 30 s for two heartbeats.

- **Look for:** both eyes blink together; the smile never changes shape;
  cheek bars move only while speaking (R4); every state change appears
  at once, with no half-drawn face; Sleeping, Error and Ready look as before.
- **Record:** `[cap] … maxPush=…us maxTrans=…us`, `[hb] … maxLoopSteady=…us
  maxLoopTransition=…us`, `[pb] … maxLoopSpeaking=…us`, and the `[blocks]`
  lines (`face render`, `mem.checkpoint`) — plus any `face render` alert or
  `steady-state loop exceeded` line.
- **Expected** (not a pass/fail threshold change): `maxTrans` ≈ 25 ms,
  `maxPush` and `maxLoopSteady` up to ≈ 20 ms; face alerts on some
  transitions are possible and must be reported.
- **Fail:** any visual difference from before, or any capture/playback loss
  in R4.

**R8 — collect (Window A).** Ctrl+C in the monitor, then:

```powershell
Pop-Location
$log = Get-ChildItem $env:TEMP -Filter "platformio-device-monitor-*.log" | Sort-Object LastWriteTime | Select-Object -Last 1
Select-String -Path $log.FullName -Pattern "\[face\]|face sprites|\[mem\]|low-water|tlsAllocFail|maxPush|maxLoop|face render|exceeded|SUMMARY|TTH Bot - M5Stack Core2|Guru Meditation" | ForEach-Object { $_.Line }
```

**Report back** that output (it contains no secrets), then delete the log with
`Remove-Item $log.FullName`.

**Overall pass:**
- free ≥ 32000 and largest ≥ 24000 at R2–R6;
- `tlsAllocFail=0`;
- no reboot (one banner, at the start);
- capture and playback intact;
- face behaviour unchanged.

Every `[mem] new internal low-water` line is reported with its stage for the
attribution of any transient. If R4 or R6 fails, keep the log; no
configuration change is needed to recover.

---

## Gateway turns (Step 6.3)

**Status: CLOSED for v1 (product-owner decision, accelerated v1 path).**

Physically confirmed on the Core2 (production firmware, LAN gateway with
`GEMINI_MODE=fake`):

- production firmware boot and gateway authentication;
- Wi-Fi, pinned-CA TLS and the WebSocket connection;
- upstream microphone audio through `GatewayTurnSource`;
- the fake-gateway echo returned as 24 kHz audio;
- exact 16 kHz → 24 kHz duration conversion;
- downstream credit reaching zero and resuming;
- the ring draining back to zero;
- credit returning to 192 000 B;
- barge-in stopping playback and starting a new capture;
- the first-response timeout;
- the connection remaining usable after the timeout;
- a gateway restart followed by a successful turn;
- memory remaining above the approved limits (current internal free
  ≥ 32 000 B, largest block ≥ 24 000 B);
- no credit violations, bad frames, send failures, capture drops or reboots.

**Known accepted issue — playback underruns.** Occasional underruns were
measured, including one completed response with `underruns=1` and one
cancelled response with `underruns=9`. Not every playback turn had zero
underruns. No blocking functional failure was perceived, and the issue is
accepted for v1. Planned post-v1 improvement: a startup prebuffer before
playback begins. No code was changed for it.

**Not physically completed — skipped by product-owner decision, not passed:**

- deterministic session loss during active playback (P5.5);
- P7, the long-response physical stress test;
- P14, the physical credit soak and the intentional over-credit test.

These paths remain covered by the automated tests where applicable
(`test_gateway_turn`: session loss in LISTENING / WAITING / SPEAKING, a
response 2.5× the ring with credit conservation, over-credit refusal;
`test_downstream_ring`; the gateway's `fake_echo_test.ts` for the hold and
over-credit controls).
Nothing is deployed: the gateway runs on this PC with `GEMINI_MODE=fake`, whose
"model speech" is an **echo of the child's own captured audio**, converted by
the gateway from 16 kHz to 24 kHz (the firmware has no resampler).

What changed on the robot:

- **Production always uses the gateway turn source.** The Phase 5 local mock
  and its keys (`m`, `p`, `x`, `g`, and the new `t`) exist only in a
  diagnostic build (`-DTTH_DIAGNOSTIC_BUILD=1`); a production image has no key
  that switches away from the gateway.
- **Up:** the child's speech streams while it is recorded, as 320-sample
  (20 ms, 640 B) frames through ONE ordered outbound queue (PSRAM, shared with
  the network task under a mutex): `turn_start` before its audio, all accepted
  audio before `turn_end`. An item leaves the queue only after the whole frame
  was written; a failed or partial write fails the turn and reconnects.
- **Down:** model audio arrives in frames of at most **1920 B = 960 samples =
  40 ms at 24 kHz** (one playback slot) into a **192 000 B** PCM ring
  (290 304 B in PSRAM with frame headers). Credit is returned as audio is
  consumed, in **3840 B batches = two frames**. The network task never waits:
  a frame over credit or ring capacity is refused, the turn fails once, and
  the robot reconnects with clean accounting.
- **First-response timeout: 10 s**, started only when `turn_end` has been
  completely written to the socket. On expiry the turn fails once
  (`no response in time`), everything is released and returned, the face
  shows ERROR for 3 s and returns to Ready; the connection stays up.
- **Session loss** (gateway stopped, Wi-Fi lost, pong timeout, protocol
  error): the active turn fails once, capture and playback stop, the bus is
  released, ERROR for 3 s, then Sleeping until the robot has reconnected.
- **Barge-in** during gateway speech: playback stops, the turn's buffered audio
  is purged and its credit returned, `cancel` is queued, the microphone
  starts, and only then LISTENING. No acknowledgement is awaited.

New serial lines (every heartbeat, one per loop iteration; no audio, prompt,
token or credential is ever printed):

```text
[turn] source=gateway phase=idle started=… ok=… failed=… cancelled=… refused=… | upBusy=… endBusy=… outboundHigh=…/32
[turn] firstResponseTimeouts=… sessionLossFailures=… protocolFailures=… gatewayErrors=… staleEvents=… interrupted=… firstResponse=…/…ms
[credit] granted=192000 spent=… consumed=… returned=… left=… | ring used=… high=… of 290304 B | returnedTotal=… returnBusy=…
[credit] zeroStalls=… creditViolations=… (capacity …) staleB=… cancelledB=… oldConnB=… | sent=… sendFail=… down=… badFrames=…
```

and, at each point of a turn (M3/M4),
`[mem] M3/M4 <point>: internal free=… largest=… (historical min=…) | ring=…B credit left=… zeroStalls=… | tlsAllocFail=…`
with `<point>` = `capture/upstream`, `waiting`, `downstream buffering`,
`playback`, `zero-credit stall` (at most one line per 2 s) and `back to ready`.

`zeroStalls` counts frames that left the gateway **exactly 0 B** of credit;
`spent` / `consumed` / `returned` are per connection (they restart at each
connect).

### Physical test procedure — P5, P7, P14, M3, M4

*The Step 6.3 campaign was stopped on the accelerated v1 path. The procedure
below is kept for reference; P5.5, P7 and P14 were not run (see the status
above), and it is not required for v1.*

The robot is on **COM3**. Windows A (robot), B (gateway) as in Step 6.2.
**Leave the serial monitor with Ctrl+C before every upload.** No command below
contains a secret or a value to edit.

**Prerequisites from Step 6.2 (already done on this PC, not repeated):**
Stage 3 (dev CA and certificate), Stage 4 (robot provisioned), Stage 5
(`gateway/.env`), Stage 6 (firewall rule). In a new Window A run Step 6.2's
Stage 0 (session variables); nothing else is needed.

Every gateway start below is in **Window B** and begins by clearing all
earlier test settings, so no control can leak from one step into the next.
A variable set in the shell takes precedence over `gateway/.env`; nothing is
written to disk.

#### Stage 1 — production firmware (Window A)

```powershell
cd d:\tth-bot\tth_bot
Test-Path Env:PLATFORMIO_BUILD_FLAGS
& $pio run -d firmware\core2 -e core2-touch -t upload --upload-port COM3
```

- **Expected:** `False`, then `[SUCCESS]`.
- **Fail:** `True` → `Remove-Item Env:PLATFORMIO_BUILD_FLAGS` and repeat.

#### Stage 2 — gateway with the default echo (Window B)

```powershell
cd d:\tth-bot\tth_bot\gateway
Remove-Item Env:FAKE_* -ErrorAction SilentlyContinue
D:\deno\deno.exe task start:lan
```

- **Expected:** `"event":"gemini_mode","mode":"fake"`,
  `"event":"fake_echo","ms":300,"count":1`,
  `"event":"fake_credit_controls","ms":0,"count":0`, `"event":"listening","count":8443`.

#### Stage 3 — boot (Window A)

```powershell
& $pio device monitor --port COM3 --baud 115200
```

- **Manual:** press the Core2's reset button.
- **Expected:**

  ```text
    TTH Bot - M5Stack Core2 - PHASE 6.3 gateway turns (TLS WebSocket; speech via the gateway)
  [gw] network task on core 0 (priority 2, stack 8192 B); TLS with the pinned CA and host name, no insecure mode
  [gw] downstream ring 290304 B (credit 192000 B + 8192 x 12 B frame headers) at 0x3f…… PSRAM | outbound queue … B at 0x3f…… PSRAM
  [gw] down frames <= 1920 B (960 samples = 40 ms at 24 kHz = one playback slot); credit returned in 3840 B batches; first-response timeout 10000 ms
  [gw] timings: production build - proactive reconnect 3300 s, turn guard 3480 s, ping 15 s, pong 10 s, auth retry 300 s
  [turn] source: gateway (tth.v1); this image has no local mock
  …
  [gw] hello sent (proto 1, fw core2-6.3, credit 192000 B)
  [gw] READY session=… (sessions 1)
  [app] state connecting -> ready (face: ready)
  ```

- **Pass:** both allocations say `PSRAM`, `production build`, `READY`.
- **Fail:** `[gw] *** network task could not start: gateway disabled ***` →
  the PSRAM allocation was refused; report the boot log. For every other
  connection failure use Step 6.2 Stage 8's table.

**No mock in production.** Type `m`, `p`, `x`, `g`, `t` in the monitor, then `?`.
- **Expected:** nothing for the five letters; `?` prints only
  `[keys] face: r=ready l=listening w=waiting s=speaking e=error o=sleeping a=cycle b=bar level | ?=help`
  and the provisioning line.
- **Pass:** no `[turn] mock …`, `[loopback] …` or `DIAGNOSTIC` line.

#### Stage 4 — P5: a gateway turn end to end

**P5.1 One turn.** Hold the centre touch button, speak for about 3 s, release.

- **Expected (robot), in this order (other lines may interleave):**

  ```text
  [ptt] PRESS
  [stream] streaming to gateway: 320-sample frames
  [app] state ready -> listening (face: listening)
  [mem] M3/M4 capture/upstream: internal free=… largest=… (historical min=…) | ring=0B credit left=192000 zeroStalls=0 | tlsAllocFail=0
  [ptt] RELEASE after … ms
  [app] state listening -> waiting (face: waiting)
  [mem] M3/M4 waiting: …
  [stream] DONE samples=S bytes=… frames=F busyRetries=… maxLag=…B committed=S | gateway queued frames=F bytes=…
  [gw] speech_start turn 1
  [turn] speech start: 24000 Hz mono s16le (gateway)
  [mem] M3/M4 downstream buffering: …
  [play] stream open: 24000 Hz mono s16le, 3 x 960-sample slots, …
  [app] state waiting -> speaking (face: speaking)
  [play] first audio accepted by speaker … ms after speech start
  [mem] M3/M4 playback: …
  [gw] turn_complete turn 1 frames=D bytes=B
  [turn] complete: gateway delivered B/2 samples
  [app] state speaking -> ready (face: ready)
  [mem] M3/M4 back to ready: …
  ```

- **Expected (gateway):** `turn_start` (turn 1), `turn_end` with the same
  `frames` F and `bytes` as the robot's `[stream] DONE … gateway queued`,
  then `response_complete` with `frames` D and `bytes` B.
- **Manual:** you hear your own words at their normal speed and pitch.
- **Pass:** robot and gateway agree on F, D and B; `B` is 3 × the uploaded
  bytes ÷ 2 (rounded down to a whole sample); the voice is natural; the next
  heartbeat shows `[turn] … ok=1 failed=0` and
  `[credit] granted=192000 spent=B consumed=B returned=B left=192000` with
  `creditViolations=0 staleB=0 cancelledB=0 badFrames=0`.

**P5.2 Five turns.** Repeat P5.1 four more times.
- **Pass:** `[turn] … started=5 ok=5 failed=0`, `left=192000` between turns,
  and `[gw] state=ready … sessions=1` (no reconnect).

**P5.3 Barge-in during gateway speech.** Window B: Ctrl+C, then

```powershell
Remove-Item Env:FAKE_* -ErrorAction SilentlyContinue
$env:FAKE_ECHO_REPEAT = "3"
D:\deno\deno.exe task start:lan
```

Wait for `[gw] READY … (sessions 2)`. Speak for about 3 s and release; while
the robot is repeating you (about 9 s), press and hold again, say one short
word, release.

- **Expected (robot):**
  `[barge] press while speaker active (player streaming): stopping playback first`,
  `[barge] listening: speaker quiet after … ms, press->mic … us`,
  `[app] state speaking -> listening`, then the new turn: `speech_start turn N+1`
  and its echo. Nothing of the first answer is heard after the press.
- **Expected (gateway):** `turn_cancelled` for the first turn, `turn_start` for the next.
- **Pass:** as above; the heartbeat shows `cancelled=1`, `cancelledB=` > 0,
  `creditViolations=0`, and after the second answer `left=192000`.

**P5.4 First-response timeout.** Window B: Ctrl+C, then

```powershell
Remove-Item Env:FAKE_* -ErrorAction SilentlyContinue
$env:FAKE_FIRST_RESPONSE_DELAY_MS = "11000"
D:\deno\deno.exe task start:lan
```

Wait for `READY … (sessions 3)`. Speak for 2 s and release; wait 15 s without
touching anything.

- **Expected (robot), about 10 s after `[stream] DONE`:**

  ```text
  [turn] ERROR from source: no response in time
  [turn] FAILED: no response in time (ERROR for the hold, then back to rest)
  [app] state waiting -> error (face: error)
  [app] state error -> ready (face: ready)
  ```

  (about 3 s between the last two lines). No `[gw] connection closed`.
- **Then** speak another 2 s turn: it is answered normally (only the first
  response of the gateway process is delayed).
- **Pass:** `firstResponseTimeouts=1`, `failed=1`, `ok` +1 after the second
  turn, `left=192000`, and `[gw] … sessions=3` unchanged (the connection
  survived the timeout).

**P5.5 Session loss while speaking.** Window B: Ctrl+C, then

```powershell
Remove-Item Env:FAKE_* -ErrorAction SilentlyContinue
$env:FAKE_ECHO_REPEAT = "3"
D:\deno\deno.exe task start:lan
```

Speak for 3 s and release; while the robot is repeating you, Window B: Ctrl+C.

- **Expected (robot):** `[gw] connection closed: …`,
  `[turn] gateway session ended while responding: turn fails once, robot reconnects`,
  `[turn] ERROR from source: gateway session lost`,
  `[app] state speaking -> error (face: error)`, then about 3 s later
  `[app] state error -> disconnected (face: sleeping)` (or `-> connecting`),
  with `[gw] … retry in …`.
- **Restore:** Window B: `D:\deno\deno.exe task start:lan`.
- **Pass:** `READY` again without a reboot, `sessionLossFailures=1`, and the
  next turn is answered normally.

P5 passes when P5.1–P5.5 all pass.

#### Stage 5 — P7: a response four times longer than the ring

Window B: Ctrl+C, then

```powershell
Remove-Item Env:FAKE_* -ErrorAction SilentlyContinue
$env:FAKE_ECHO_REPEAT = "4"
D:\deno\deno.exe task start:lan
```

Wait for READY. Hold the button and **count aloud steadily for about 6 s**,
release. The answer is about 24 s of audio (≈ 1 152 000 B, six times the
192 000 B ring). Keep the monitor running until `back to ready`.

- **Expected (robot):** `[mem] M3/M4 zero-credit stall: …` lines every few
  seconds during the answer; heartbeats during the answer with
  `[credit] … left=` between 0 and a few thousand and `ring used=` near 190 000;
  at the end `[gw] turn_complete turn N frames=D bytes=B`,
  `[turn] complete: gateway delivered B/2 samples`, `[play] SUMMARY1 end=completed … underruns=… …`.
- **Manual:** your counting is heard four times in a row, at normal speed,
  without gaps, clicks or repeats.
- **Pass (all):**
  1. **stops exactly at zero:** `zeroStalls` rose during the answer, and
     `creditViolations=0`;
  2. **faster than real time until zero:** `[credit] … high=` is at least
     193 200 (the whole 192 000 B credit was filled, with its frame headers);
  3. **resumes after consumption, no underrun after priming:** the
     `[play] SUMMARY1` line shows `underruns=0`, and the answer never paused;
  4. **complete, no loss or duplication:** robot `bytes=B` equals the
     gateway's `response_complete` bytes, the delivered samples are B/2, and
     `staleB=0 cancelledB=0 oldConnB=0 badFrames=0`;
  5. **accounting restored:** after `back to ready`, the next `[credit]` line
     shows `spent` = `consumed` = `returned` and `left=192000`.

#### Stage 6 — P14: credit under test controls

**P14.1 Valid soak.** Keep the Stage 5 gateway. Do **10 turns** like Stage 5
(count for 5–6 s each), one after another, over at least 5 minutes.
- **Pass:** every heartbeat shows `creditViolations=0`; `zeroStalls` rises
  with each answer; every answer completes (`ok` +10, `failed=0`);
  `[gw] … sessions=` unchanged; `underruns=0` in every playback SUMMARY.

**P14.2 Forced zero-credit hold; control messages at zero audio credit.**
Window B: Ctrl+C, then

```powershell
Remove-Item Env:FAKE_* -ErrorAction SilentlyContinue
$env:FAKE_ECHO_REPEAT = "4"
$env:FAKE_CREDIT_HOLD_MS = "2000"
D:\deno\deno.exe task start:lan
```

Wait for READY. Count aloud for 6 s, release, and let the answer play out.

- **Expected (gateway):** `"event":"fake_credit_controls","ms":2000,"count":0`
  at start; during the answer repeated `"event":"test_credit_hold","ms":2000`.
- **Expected (robot):** `zero-credit stall` lines; the `[gw]` heartbeat during
  the answer keeps `pings` rising with a fresh `rtt=` and **no** `pong timeout`
  or `connection closed` — pongs are delivered while audio credit is zero.
- **Pass:** the hold is visible in Window B; the answer still completes with
  the same byte checks as P7 (4 and 5), `underruns=0`, `creditViolations=0`.
  (Each 2 s hold is shorter than the 4 s the ring holds, so playback never
  runs dry.)

**P14.3 Intentional over-credit frame.** Window B: Ctrl+C, then

```powershell
Remove-Item Env:FAKE_* -ErrorAction SilentlyContinue
$env:FAKE_ECHO_REPEAT = "4"
$env:FAKE_VIOLATE_CREDIT = "1"
D:\deno\deno.exe task start:lan
```

Wait for READY. Count aloud for 6 s and release.

- **Expected (gateway):** at the first zero-credit stall one
  `"event":"test_over_credit_frame"` with the turn and `"bytes":1920`, then
  `session_closed`.
- **Expected (robot):**

  ```text
  [gw] *** downstream credit violation on turn N: frame refused, nothing buffered was dropped; turn fails, reconnecting ***
  [turn] ERROR from source: protocol violation
  [turn] FAILED: protocol violation (ERROR for the hold, then back to rest)
  [gw] connection closed: protocol error, code 1008, after … s
  [app] state speaking -> error (face: error)
  …
  [gw] READY … (sessions +1)
  ```

- **Then:** count aloud for 6 s once more: the answer plays completely (the
  gateway sends only one bad frame per run).
- **Pass:** `creditViolations=1` (and it stays 1 afterwards),
  `protocolFailures=1`, `sessionLossFailures` unchanged (the turn failed once,
  by the violation), READY again without a reboot, and the second answer
  passes P7's checks 1–5.

**Restore** (Window B): Ctrl+C, then
`Remove-Item Env:FAKE_* -ErrorAction SilentlyContinue`.

#### M3 and M4 — memory during gateway turns

Collected from Stages 4–6; nothing extra to run. Copy every
`[mem] M3/M4 …` line and the `[mem]` heartbeat lines.

- **M3 (capture and upstream, waiting):** the `capture/upstream` and `waiting` points.
- **M4 (downstream):** the `downstream buffering`, `playback`,
  `zero-credit stall` and `back to ready` points.
- **Pass (every point):** current `internal free` ≥ **32 000 B** and
  `largest` ≥ **24 000 B**; `tlsAllocFail=0`; no reboot banner during the
  whole procedure; `back to ready` free does not trend down across P14.1's ten
  turns (record the first and the last). The `historical min=` figure is
  recorded separately and is not the gate.

Report per gate: pass/fail, and the lines named in each **Pass**.

---

## On-device activity selection

**Status: physical test PASSED (2026-09-16, product owner), with the evidence
split below.** The child chooses the conversation activity on the robot
itself — no website and no serial command.

Evidence (production firmware `core2-6.4`, real Gemini on the LAN):

- **Proven by the retained logs:** capture `failed=0 dropped=0`; playback
  `rejects=0 refusals=0`; `creditViolations=0`, `tlsAllocFail=0`; the centre
  zone did not start capture while the menu was open (`press ignored:
  activity menu open`); a menu selection on the robot
  (`select … sent` → `selected … (saved)`, recorded on the build before the
  haptics change); a saved id sent in `hello` and READY reporting that id;
  **no vibration when playback starts** (no `[haptics]` line on the current
  build); no prompt, title, participant name or credential in any log.
- **Physically confirmed by the product owner, covered by automated tests,
  not fully captured in the retained logs:** switching to a different activity
  and its persistence across a reboot on the current build, and the two-pulse
  vibration when the menu is refused while busy. The gateway's switch ordering
  (`activity_select` → `gemini_close` → `gemini_ready` → `activity_selected`)
  and a non-default restore were **not** observed in a retained log (that
  gateway log was overwritten); they are covered by
  `gateway/tests/activity_select_test.ts` and `activity_loading_test.ts`.
- One real-Gemini turn hit the 10 s first-response timeout and recovered
  (ERROR, then normal turns): a transient, not a selector failure; see
  PHASE6_PLAN §12.4.

| Touch zone (below the display) | Menu closed (READY, online) | Menu open |
|---|---|---|
| **Left**, short press | opens the menu | previous activity (wraps) |
| **Centre** | push-to-talk, unchanged | confirms the highlighted activity; never starts capture |
| **Right** | — | next activity (wraps) |

- The menu opens only while online and idle in READY (not during capture,
  waiting, playback, barge-in or a reconnect); otherwise two short pulses and
  `[activity] menu refused: <reason>`. It closes after 10 s without input.
- It shows only `2/4` and the title, reduced to ASCII for the built-in font
  (`Conversație liberă` → `Conversatie libera`). The device receives only
  activity ids, titles (≤ 48 UTF-8 bytes) and modes — never a prompt or
  participant data.
- Confirming another activity sends `activity_select`; the screen shows
  `Schimb activitatea...` while the gateway loads a fresh snapshot, closes the
  Gemini session and opens a new one. Only after the new session is ready does
  it answer `activity_selected`, the robot saves the id in NVS and the face
  returns. A refusal (`activity_select_error`) or no answer within 30 s shows
  `Nu s-a putut schimba` for 2 s; the previous activity stays in use.
- After a reboot the saved id is sent in `hello`; the gateway restores it, or
  keeps its configured activity and says why (a missing, disabled or invalid
  saved activity is then forgotten).
- Robots with older firmware keep the gateway's configured activity.
- **Haptics (same firmware):** the robot no longer vibrates when it starts
  speaking (the 120 ms first-audio pulse is removed). The motor is used only
  for the two short pulses of a refused action; the `[pb]` heartbeat reports
  them as `hapticsRefused=N`.
- Heartbeat: `[activity] current=<id> saved=<id|-> list=N selector=<state>
  selected=N failed=N timeouts=N listErrors=N`.

## Phase 0 results — PASSED

Verified on the physical original M5Stack Core2:

| Check | Result |
|---|---|
| Board detected | ✅ correct |
| Touch PTT press / hold / release (`M5.BtnB`) | ✅ works |
| Microphone capture | ✅ speech captured clearly |
| Speaker tone | ✅ audible |
| Vibration motor | ✅ felt |
| Stability | ✅ no crashes or resets |
| Usable PSRAM | ~4 MB (not the 8 MB the datasheet implies) |

One defect was found and is fixed in Phase 1: the diagnostic called
`M5.Mic.end()` / `M5.Speaker.end()` unconditionally to "reset" audio state,
which produced `I2S port 1 has not installed` from the ESP-IDF driver. See
*Audio ownership* below.

### Locked-in decisions

1. **Microphone and speaker are strictly half-duplex.** Only one may be
   installed at any moment.
2. **`AudioBus` is the exclusive owner** of every mic/speaker `begin()` and
   `end()` transition.
3. **No no-op `AudioBus`** — even though the Phase 0 probe reported that full
   duplex *appeared* possible. The two peripherals are driven from the same
   I2S peripheral and share a clock pin; a probe that does not glitch is not
   evidence that sustained simultaneous use is safe. Push-to-talk is naturally
   half-duplex, so the guarantee costs nothing and removes a whole class of
   intermittent audio faults. Barge-in still works, because it is sequential.
4. **No unmatched I2S uninstalls.** `end()` is only ever called for a
   peripheral whose `begin()` returned true.
5. **Design against ~4 MB PSRAM**, the measured figure, not the datasheet's.
   `TTH_PSRAM_MIN_BYTES` makes the startup report fail loudly on less.
6. **The Flutter and Supabase implementations stay unchanged.**
7. **Both `core2-touch` and `core2-sw201` stay buildable** at all times.
8. **`M5.BtnB`** (centre capacitive touch) is the active development input.

---

## Audio ownership

`lib/tth_core/include/tth/AudioBus.h` is the boundary. `M5AudioDevice`
(`src/audio/`) is the only file in the firmware permitted to call
`M5.Mic.begin/end` or `M5.Speaker.begin/end`, and only `AudioBus` calls it.

`AudioBus` guarantees, and the host tests pin, that:

- the mic and speaker are never installed simultaneously;
- switching owners ends the outgoing peripheral **before** starting the
  incoming one;
- `releaseAll()` on an unowned bus issues **no** hardware call at all — this is
  what makes the Phase 0 `I2S port 1 has not installed` error impossible;
- a failed `begin()` leaves the bus unowned rather than silently restoring what
  was just torn down, so callers must handle the failure. In particular,
  barge-in must not show the LISTENING face when `acquireMic()` returns false.

---

## Layout

```
firmware/core2/
  platformio.ini
  include/tth/Config.h                 compile-time config; the ONLY file that
                                       differs between the two device builds
  lib/tth_core/                        PORTABLE core — no M5Unified, no
    include/tth/AppState.h             Arduino, no ESP32 header, ever
    include/tth/FaceState.h
    include/tth/AudioBus.h             half-duplex arbitration
    include/tth/IPushToTalkInput.h     the PTT interface
    include/tth/PttButton.h            debounce, edges, 45 s max hold
    include/tth/ConversationStateMachine.h
    include/tth/FaceFrame.h            one snapshot, one eyeOpenness
    include/tth/FaceGeometry.h         ported layout + eyeRenderFor()
    include/tth/FaceAnimator.h         blink, drift, expression, smoothing
    include/tth/FaceOverride.h         serial diagnostic override
    include/tth/IAudioCapture.h        the microphone data path
    include/tth/CaptureController.h    one recording, start to finish
    include/tth/TurnBuffer.h           preallocated, never resized
    include/tth/PcmProcessing.h        DC removal + levels
    src/*.cpp
  src/                                 device-only code (M5Unified lives here)
    main.cpp                           thin entry point
    app/App.{h,cpp}                    init, cooperative loop, wiring
    audio/M5AudioDevice.{h,cpp}        the only M5.Mic/M5.Speaker begin/end
    audio/M5MicrophoneCapture.*        async M5.Mic.record adapter
    input/TouchPushToTalkInput.*       M5.BtnB adapter
    input/PhysicalPushToTalkInput.*    SW201 / GPIO 33 adapter
    input/PushToTalkInputFactory.*     the ONE place a concrete type is named
    ui/FaceRenderer.{h,cpp}            2 PSRAM sprites, atomic transaction push
    diag/StartupReport.{h,cpp}         boot report + health heartbeat
  test/                                host unit tests (native environment)
    test_app_state/                     5 cases
    test_audio_bus/                    11 cases
    test_ptt_button/                   15 cases
    test_conversation_state/           13 cases
    test_face_geometry/                16 cases
    test_face_animator/                17 cases
    test_lower_face/                   30 cases
    test_turn_buffer/                  14 cases
    test_capture/                      20 cases
```

`lib/tth_core` is compiled unchanged for the device **and** for the host test
environment, which is exactly what keeps "portable code has no hardware
dependency" true rather than aspirational — the `native` build fails
immediately if anyone slips an `#include <M5Unified.h>` into it.

### Environments

| Environment | Purpose |
|---|---|
| `core2-touch` *(default)* | Hold-to-talk on `M5.BtnB`, the **centre** capacitive touch zone. Active development configuration. |
| `core2-sw201` | Hold-to-talk on a normally-open SW201 between **GPIO 33 and GND** (internal pull-up, active low). Built every time so it cannot rot. |
| `native` | Host unit tests for `lib/tth_core`. |

Strict warnings (`-Wall -Wextra -Werror`) are scoped to our own sources via
`build_src_flags`, not applied globally — M5Unified and M5GFX emit several
unused-variable warnings that we cannot fix upstream and must not silence
project-wide, since that would weaken the checks on our own code too.

#### SW201 wiring (for later)

```
SW201 pin 1 ── GPIO 33 (Port A, red connector, SCL line)
SW201 pin 2 ── GND
```

No external resistor: `INPUT_PULLUP` idles the pin high, the button pulls it
low. **GPIO 33 is Port A's SCL**, so the SW201 and any Port A I²C unit are
mutually exclusive.

---

## One-time setup on this machine

Three environment problems had to be worked around. None is a property of the
project.

### 1. PlatformIO core directory (REQUIRED — builds fail without it)

The Windows username is `Tales&Tech`. PlatformIO emits linker search paths
**unquoted**, and `cmd.exe` treats `&` as a command separator, so the link step
loses its `-L` directories and fails with `cannot open linker script file
esp32.rom.redefined.ld`. This affects *any* ESP32 project on this machine.

A directory junction gives an `&`-free path to the same files (no re-download;
deleting the junction deletes nothing):

```powershell
New-Item -ItemType Junction -Path "D:\pio-core" -Target "$env:USERPROFILE\.platformio"
```

### 2. TLS interception by Avast (only when downloading packages)

Avast intercepts HTTPS, and PlatformIO's bundled Python trusts `certifi` only,
so downloads fail with a bare `HTTPClientError`. A combined bundle lives at
`D:\pio-core\ca-bundle.pem`. To regenerate:

```powershell
Get-Content "$env:USERPROFILE\.platformio\penv\Lib\site-packages\certifi\cacert.pem",
            "C:\ProgramData\Avast Software\Avast\wscert.pem" |
  Set-Content -Encoding ascii "D:\pio-core\ca-bundle.pem"
```

### 3. Host compiler for the `native` tests

There is no system C/C++ compiler, and PlatformIO's bundled SCons has MSVC
support stripped out (`No module named 'SCons.Tool.MSCommon'`), so the MSVC
that ships with Visual Studio cannot be used. Portable MinGW-w64 (niXman
mingw-builds, GCC 16.2.0, UCRT) is extracted to `D:\mingw64` — no installer, no
change to the system `PATH`.

**Avast quarantines `D:\mingw64\bin\ld.exe`** — a well-known false positive on
MinGW's linker. It is removed within seconds of being written, so re-extracting
it does not help:

```
collect2.exe: fatal error: cannot find 'ld'
```

**This is already worked around and needs no action.** Avast matches on the
name `ld.exe` only; `ld.bfd.exe` — the same BFD linker under its versioned
name — is untouched. The `native` environment therefore passes `-fuse-ld=bfd`,
which selects it explicitly. That flag is portable (every GCC and MinGW install
ships `ld.bfd`), so it costs nothing anywhere else.

If you would rather have the stock setup, restore `ld.exe` from Avast's
quarantine and add `D:\mingw64` to **Settings → General → Blocked & Allowed
apps**; the `-fuse-ld=bfd` flag will keep working either way.

### Set these per PowerShell session

```powershell
$env:PLATFORMIO_CORE_DIR = "D:\pio-core"
$env:REQUESTS_CA_BUNDLE  = "D:\pio-core\ca-bundle.pem"
$env:PATH                = "D:\mingw64\bin;$env:PATH"
```

Or make them permanent (affects new terminals only):

```powershell
[Environment]::SetEnvironmentVariable("PLATFORMIO_CORE_DIR", "D:\pio-core", "User")
[Environment]::SetEnvironmentVariable("REQUESTS_CA_BUNDLE",  "D:\pio-core\ca-bundle.pem", "User")
```

---

## Build, test, upload (Windows)

`pio.exe` is not on `PATH`; use the call operator with the full path.

```powershell
cd d:\tth-bot\tth_bot\firmware\core2
$env:PLATFORMIO_CORE_DIR = "D:\pio-core"
$pio = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"

# Host unit tests (no hardware needed) - 547 test cases, 37 suites
$env:PATH = "D:\mingw64\bin;$env:PATH"
& $pio test -e native

# Both device builds
& $pio run -e core2-touch
& $pio run -e core2-sw201

# Flash and watch
& $pio device list                                  # find the COM port
& $pio run -e core2-touch -t upload -t monitor      # 115200 baud
```

If the port is not auto-detected:

```powershell
& $pio run -e core2-touch -t upload --upload-port COM3
& $pio device monitor --port COM3 --baud 115200
```

Leave the monitor with **Ctrl+C**. If the Core2 does not appear as a COM port,
install the CH9102 / CP210x USB-UART driver and use a data-capable USB-C cable.

---

## The push-to-talk abstraction

`IPushToTalkInput` (portable) is what the rest of the firmware sees.
`pushToTalkInput()` in `src/input/PushToTalkInputFactory.cpp` is the **one**
place a concrete implementation is named; the choice comes from `TTH_PTT_INPUT`,
which `platformio.ini` sets per environment. Nothing else may name a concrete
type — that is what keeps the SW201 swap a configuration change.

Both implementations are thin adapters over the portable `PttButton`, which
holds *all* the timing behaviour: debouncing, one-shot edges, hold duration and
the 45 s maximum-hold ceiling. They differ only in how the raw "is it down?"
boolean is obtained, so they cannot drift apart:

| | `TouchPushToTalkInput` | `PhysicalPushToTalkInput` |
|---|---|---|
| Source | `M5.BtnB.isPressed()` | `digitalRead(33) == LOW` |
| Debounce | 0 ms — M5Unified already debounces the FT6336U | 30 ms |
| Max hold | 45 s | 45 s |

`PttButton` takes time as a parameter instead of reading a clock, so the 45 s
timeout is tested by passing `45000` — no waiting, no hardware.

A forced release also **locks out** the input until the button is genuinely let
go. Without that, a stuck or short-circuited button would re-arm every 45 s
forever.

## Phase 1 and 2 validation — PASSED

Phase 1, on the physical device: `STARTUP REPORT: ok`, board detected as
`board_M5StackCore2`, audio duplex `HALF (strict)`, `AudioBus` owner `none` at
boot, **no I2S errors**, no loop-timing warnings, no resets. Over 440 s: heap
flat at 325 380 bytes, PSRAM flat at 4 192 123 bytes, `maxLoop` ~386–387 µs.

Phase 2: press/hold/release correct, `READY → LISTENING → WAITING → READY`
correct, the 45 s forced release fires while still held, the input stays locked
out until genuine physical release, a subsequent press works, no false presses,
`AudioBus` unowned throughout, heap flat at 325 324 bytes.

---

## The robot face

Two eyes and one mouth on `#060F1A`, in `#4DEFFF` (`#2C6E78` when in error).
Nothing else ever reaches the display — no text, icons, status labels, borders
or menus.

### Layout

`FaceGeometry` ports `RobotFaceGeometry.fromWidth(340)` from Flutter: the same
reference canvas, the same ratios, the same `BoxFit.contain` scale, targeting
320×240 instead of a phone screen.

At 320×240 the fit is width-limited (`320/340 = 0.9412`), giving:

| | Value |
|---|---|
| Eye radius | 49.6 px |
| Eye centres | x = 88 / 232, y = 66 |
| **Eyes sprite (both eyes)** | **260×116 at (30, 8)** — 30 160 px |
| Fixed smile | 147.2 × 73.6 px, centre (160, 191) |
| Bar centres | x = 52, 64, 76 │ 244, 256, 268 · y = 197 |
| Bar half-heights (outer→inner) | 13, 17, 21 px |
| **Lower-face sprite (smile + bars)** | **226×46 at (47, 174)** — 10 396 px |
| **Total sprite PSRAM** | **81 112 bytes (~79 KB)** |
| Full-face SPI cost | 40 556 px → **16.2 ms** at 40 MHz, 16bpp |

### Both eyes are ONE sprite

The two eyes share a single sprite, drawn from a single `FaceFrame` via
`eyeRenderFor()` and pushed in a single operation. There is exactly one
`eyeOpenness` value in the whole system, so the eyes are structurally incapable
of showing different blink phases.

This replaced an earlier two-sprite design in which only the left eye visibly
blinked: `render()` pushed at most one sprite per call and returned, and
because the eye parameters changed on *every* frame of a blink, the left eye
was re-dirtied and pushed each time while the right eye — next in priority —
was never reached. It caught up only after the blink had finished, by which
point openness was back to 1.0. The same starvation made state changes crawl
across the face one element per iteration.

### Rendering

Two sprites in PSRAM, never a framebuffer. `render()` redraws only the regions
whose own parameters changed, then pushes **all** of them inside one
`startWrite()` / `endWrite()` transaction, in one loop iteration. There is no
per-call push budget, so a transition can never be left half-rendered.

Because the smile is now a constant, the lower-face sprite is dirty only when
the bar level changes or the face enters/leaves the dim error colour. Most
state transitions therefore push the eyes alone (12.1 ms); the full 16.2 ms
only happens entering or leaving SPEAKING and ERROR.

Drawing happens off-screen and is pushed in one operation, so the display never
shows a partially drawn element: no flicker, no black frame.

### Sprite placement — PSRAM, fixed at compile time (Step 6.2)

Both sprites live in **PSRAM** (`TTH_FACE_EYES_IN_PSRAM=1`,
`TTH_FACE_LOWER_FACE_IN_PSRAM=1` in `include/tth/Config.h`). The renderer
allocates each exactly there, reads the buffer address back and, if it is
anywhere else, leaves the face blank with
`[face] *** … face disabled (no fallback) ***`. The earlier "internal DRAM
first, PSRAM if that allocation fails" behaviour is gone: placement never
depends on what the heap looks like at boot.

Why: with Wi-Fi and a TLS session up, the 81 112 B of sprites in internal DRAM
left too little internal heap. The first physical Step 6.2 run measured
**28 472 B current internal free** (below the 32 KB threshold) with a
**25 588 B largest block** during playback. Details and all measurements:
[docs/PHASE6_PLAN.md §7.1](docs/PHASE6_PLAN.md).

The price is rendering time. When the sprites were last in PSRAM a full-face
transition took **25.5 ms** (16.2 ms of it SPI); eyes-only frames are
estimated at ~19 ms and cheek-bar frames at ~6–7 ms. `TTH_LOOP_WARN_MICROS`
stays at 20 ms, so slow transitions show up as `face render` alerts and in
`maxTrans` / `maxPush` / `maxLoopSteady` — reported, not hidden.

**Measured on the Core2 (Step 6.2 retest):**
- `maxTrans` ≈ 24.6–27.3 ms; `maxLoopSpeaking` up to ≈ 29.1 ms.
- Face-render calls occasionally over 20 ms, with the steady-state warning on
  some face-render frames.
- Internal free ≈ 109–155 KB (largest block ≥ 94 KB).
- No audio underruns, no capture loss, no network failure, no visible
  corruption.

The placement and the 20 ms threshold are kept as they are. Nothing about
the face itself changes: atomic transitions, one eyes sprite, the fixed smile,
the cheek bars and every state's look are as before.

### The mouth is a FIXED smile

The mouth is a small friendly closed smile and it is **identical in every
state** — READY, LISTENING, WAITING, SPEAKING and ERROR. It never opens,
stretches, deforms or changes thickness. Only its colour changes, and only for
the error face.

An animated opening mouth was implemented and rejected: on the real device it
read as a **frightened grimace**. `FaceFrame` now carries no mouth-opening
parameter at all, so there is nothing an animation could key off even by
accident.

### Speech is shown by audio level bars

Three vertical rounded bars either side of the smile, symmetrically placed at
**fixed** horizontal positions. Only their height and colour intensity animate;
the face itself does not change at all while speaking.

- Rounded ends (`fillRoundRect` with radius = half width), so there are no
  sharp rectangles at any height.
- Inner bars are taller than outer ones, which gives the group a shape rather
  than a fence.
- Tallest bar is 42 px — **52 px clear of the eye region** and comfortably
  under the mouth height.
- Nearest bar edge is **7.4 px clear of the smile**; they never touch it.
- Bars rise quickly (55 ms) and fall slowly (190 ms), so they pulse with speech
  rather than flicker.
- Levels below `kSpeechSilenceThreshold` are exact silence. As the level falls
  each bar shrinks *and* fades in colour, and disappears once it is shorter
  than it is wide. **During a silent gap only the friendly smile remains.**
- No randomness anywhere: all six bars are driven by the same smoothed level.

`SpeechLevelSmoother` is exponential in real time, so the behaviour does not
depend on the frame rate. The face runs at 25 fps.

**Simulated amplitude, Phase 3 only.** `simulatedSpeechAmplitude()` walks a
repeating 12 second cycle of three phrases — **quiet (0.30), medium (0.62),
loud (1.00)** — each speaking for 3.2 s with eased edges and followed by 0.8 s
of real silence. That demonstrates the whole range of the bars and makes the
"bars disappear, smile remains" behaviour visible on every cycle. Phase 5
replaces this one expression with the real playback amplitude.

### Behaviour per state

| State | Eyes | Lower face | Blink |
|---|---|---|---|
| READY | neutral (1.00×) | fixed smile | 2.5–6 s, occasional double |
| LISTENING | enlarged (1.12×), attentive | fixed smile | yes |
| WAITING | narrowed (0.92×), slow gaze sweep, looking up | fixed smile | no |
| SPEAKING | expressive (1.05×) | fixed smile **+ audio bars** | no |
| ERROR | small (0.90×), half-lidded, still | fixed smile, dim | no |

Blink and pupil-drift timings are ported from `robot_face.dart` and
`robot_eyes.dart`: blink every 2500 + rand(3500) ms, 150 ms closing and 150 ms
opening, 22 % chance of an immediate second blink; idle gaze drift every
4000 + rand(4000) ms over 1800 ms, ±0.08 x and ±0.05 y, eased.

`FaceAnimator` is clock-injected — it never reads a timer — so all of that is
tested on the host with no display and no waiting.

### Diagnostic keys (serial, 115200)

| Key | Effect |
|---|---|
| `r` `l` `w` `s` `e` | hold READY / LISTENING / WAITING / SPEAKING / ERROR |
| `a` | cycle through all five, 2.5 s each |
| `b` | step the bar level: 0.00 → 0.25 → 0.50 → 0.75 → 1.00 → simulated envelope (also forces the speaking face) |
| `?` | help |

These are **purely visual**. `ConversationStateMachine` is never written to.
**Any push-to-talk press clears the override** and resets the bar level to the
simulated envelope, so the device always returns to real behaviour by itself.

---

## What Phase 3 does at runtime

On boot: initialises M5Unified (landscape, unchanged from Phase 1/2), prints
the startup report and the resolved face geometry, allocates the two PSRAM
sprites, puts `AudioBus` into a known unowned state, and enters the cooperative
loop showing the READY face.

Holding the push-to-talk input drives the real state machine, and the face
follows it:

```
Ready  --press-->  Listening  --release-->  Waiting  --800 ms-->  Ready
```

State changes bypass the 25 fps rate limit and render immediately.

**The 800 ms `Waiting` hold is a temporary diagnostic stand-in.** Phase 5
replaces it with "the turn source delivered speech".

**No microphone or speaker is started.** `AudioBus` stays `none` throughout.

```
[hb] up=30s state=ready face=ready audio=none heap=... psram=... loops=... maxLoop=...us pushes=... maxPush=...us maxTrans=...us
```

`maxTrans` is the worst complete state transition — draw plus push for every
region — cumulative across the session so the worst case cannot scroll away.

---

## Physical test — Phase 3

```powershell
cd d:\tth-bot\tth_bot\firmware\core2
$env:PLATFORMIO_CORE_DIR = "D:\pio-core"
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e core2-touch -t upload -t monitor
```

**1. Boot and layout.** Two cyan eyes and one cyan smile on near-black, nothing
else. Serial `FACE GEOMETRY` must report `fits on screen: yes, boxes disjoint:
yes`, `lower box : 226x46 at (47,174)` and `full face : 40556 px -> 16222 us`.

**2. The smile never changes — the key check.** Look at the smile, then press
`r`, `l`, `w`, `s`, `e` in turn, watching *only* the mouth. It must be pixel-for
-pixel the same shape, size, position and thickness in all five. The only
permitted difference is its colour going dim under `e`. **If the mouth changes
shape at any point, this has failed.**

**3. Audio bars at known levels.** Press `b` repeatedly. Each press steps
0.00 → 0.25 → 0.50 → 0.75 → 1.00 → simulated, printing the level on serial. At
each step check:
- three bars on the left and three on the right, **mirrored**;
- inner bars taller than outer bars;
- rounded ends — no sharp rectangles at any height;
- bars never touch the smile;
- at 1.00 the tallest bars are still well below the eyes and look modest;
- at 0.00 **no bars at all** — only the friendly smile.

**4. Simulated speech.** Press `s` and watch a full 12 s cycle at least twice.
It must show a **quiet** phrase, then a **medium** one, then a **loud** one,
each separated by a silent gap in which the bars **fade and shrink away
gradually** leaving only the smile. The bars must rise faster than they fall,
pulse calmly rather than flicker, and never move sideways. The face — eyes and
smile — must not change at all throughout.

**5. Blinking (both eyes).** Untouched in READY for ~60 s. Both eyes must close
and reopen **together**, in the same phase, over ~300 ms, at irregular 2.5–6 s
intervals, with occasional immediate double blinks. **If either eye lags, stays
open, or closes alone, this has failed.**

**6. Pupil drift.** Both pupils glide to a slightly different position every
4–8 s over ~1.8 s — same direction, same amount, same time. Eye outlines stay
put.

**7. Simultaneous transitions.** Press and release the centre touch circle ten
times quickly, watching the whole face. Eye changes must appear as one change
with no visible left-eye → right-eye sequence. Press `s` then `e` and back:
eyes and bars/colour must change together.

**8. ERROR face.** Press `e`. Eyes and smile switch to dim cyan `#2C6E78`, eyes
small and half-lidded, everything still, no bars. Clearly different from READY
but calm. **No error text on screen.**

**9. Override is not sticky.** With `e` showing, press the PTT button: the face
returns to the real state machine immediately and serial prints
`[face] override cleared by push-to-talk`.

**10. Flicker.** In a dim room, watch a blink, a transition and the bars. No
tearing, no partially drawn eye, no black flash, no half-changed face.

**11. Timing.** Every heartbeat must show `maxLoop` under 20 000 µs with no
`*** loop iteration exceeded ***`. **Report `maxTrans`** after exercising
transitions — expected around 16–19 ms. `heap` and `psram` must stay flat;
PSRAM sits ~79 KB below the Phase 2 figure. Leave `a` cycling several minutes
and confirm neither drifts.

---

## Microphone capture (Phase 4)

Mono PCM, signed 16-bit little-endian, 16 000 Hz — the same format the Flutter
app captures, so both embodiments feed the gateway identical audio later.

### The turn buffer

| | |
|---|---|
| Maximum hold | 45 s (`TTH_PTT_MAX_HOLD_MS`) |
| Raw requirement | 16 000 × 2 × 45 = **1 440 000 bytes** |
| Safety margin | 1 s = 32 000 bytes |
| **Allocated** | **1 472 000 bytes (736 000 samples), PSRAM** |
| Chunk buffer | 512 samples = 1 024 bytes, **internal** RAM |

Allocated **once** at start-up with `heap_caps_malloc`. Nothing allocates,
frees or resizes during a turn: a malloc stall mid-recording would drop audio
the child has already spoken, and a failure there would be unrecoverable.

The turn buffer lives in PSRAM because 1.4 MB will not fit the ~325 KB internal
heap. The chunk buffer stays in internal RAM because the I2S DMA writes it
every 32 ms and internal RAM is markedly faster.

Against the measured ~4.19 MB of PSRAM: 81 KB face sprites + 1.47 MB turn
buffer ≈ 1.55 MB, leaving ~2.6 MB free.

The buffer is **not circular** — overwriting the start of a turn would silently
corrupt it. At capacity the recording stops with an explicit `buffer full`
reason, and anything that did not fit is *counted*, never silently discarded.
Since the buffer is sized past the maximum hold, that path is defensive: it
should be unreachable in normal use.

### Processing

Per chunk, and nothing more: **DC offset removal**, then RMS and peak measured
on the corrected samples. The Core2's PDM microphone has a standing offset that
would otherwise inflate every level reading and waste headroom.

No AGC, no noise suppression, no resampling, no encoding, no voice-activity
detection. The bytes stored are the bytes that will later be transmitted, so
anything added here would silently change what the model hears.

### Ownership and failure

`CaptureController` is the only thing that acquires the microphone for capture,
and it always releases it — on a normal stop, on a failed start, and on a read
error. Leaving the microphone installed after a fault would, on this board,
block the speaker from ever starting.

```
start : AudioBus::acquireMic()      THEN  IAudioCapture::startCapture()
stop  : IAudioCapture::stopCapture() THEN  AudioBus::releaseAll()
```

**LISTENING is entered only after the microphone is actually recording.** A
face that says "I am listening" while the microphone failed to start would be a
lie the child cannot detect. Any failure goes to ERROR instead, and a press
while in ERROR retries the turn — the only recovery a child could perform.

`M5MicrophoneCapture` turns `M5.Mic.record()` (asynchronous: hand it a buffer,
it fills in the background) into the non-blocking poll the controller expects.
It never calls `M5.Mic.begin/end` — only `M5AudioDevice`, via `AudioBus`, does.

### Diagnostics

While recording, throttled to every 500 ms:

```
[capture] 1500ms samples=24064 bytes=48128/1472000 rms=0.0412 peak=0.1875 failed=0 audio=mic maxRead=37us
```

On release:

```
[capture] SUMMARY reason=button release duration=1832ms samples=29184 bytes=58368
          rms min/avg/max=0.0021/0.0388/0.1104 peak=0.2431 chunks=57 failed=0
          dropped=0 highWater=58368/1472000 bytes audio=none
```

The heartbeat carries `cap=<bytes>/<highWater>B maxRead=<us>` so long-run drift
is visible without flooding the log.

---

## Physical test — Phase 4

```powershell
cd d:\tth-bot\tth_bot\firmware\core2
$env:PLATFORMIO_CORE_DIR = "D:\pio-core"
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e core2-touch -t upload --upload-port COM3
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" device monitor --port COM3 --baud 115200
```

**1. Boot.** `STARTUP REPORT: ok`, `turn buffer : 1472000 bytes PSRAM (46 s at
16000 Hz)`, and `[capture] turn buffer 1472000 bytes PSRAM ... chunk 512
samples (32 ms)`. PSRAM should sit ~1.55 MB below the Phase 3 free figure.

**2. A normal turn.** Hold the centre touch button and speak for ~3 s.
- `[capture] started` appears, then `ready -> listening` — in that order.
- Status lines every 500 ms with `audio=mic` and `bytes` climbing at ~32 000/s.
- **`rms` must rise clearly when you speak and fall when you stop.**
- Release → `[capture] SUMMARY reason=button release`, then
  `listening -> waiting`, then `waiting -> ready`.
- `failed=0`, `dropped=0`, and `audio=none` in the summary.

**3. Silence versus speech.** Hold quietly for 2 s, release, and note the
average RMS. Repeat while talking. The speaking average must be clearly higher.
DC removal means a silent room should read near zero, not at some fixed offset.

**4. Repeated turns.** Ten turns back to back. Every summary must start from
zero samples; `highWater` grows to the longest turn and then stops. Heap and
PSRAM must stay flat — nothing is allocated per turn.

**5. Maximum hold.** Hold for a full 45 s. Expect `[ptt] holding` every 5 s,
then `FORCED RELEASE`, a summary with `reason=maximum hold`, ~720 000 samples
(~1 440 000 bytes) and `dropped=0`. The buffer must **not** report full.

**6. The face.** LISTENING while held, WAITING on release, READY after. **The
speech bars must never move** — they are reserved for assistant playback. No
text anywhere on screen.

**7. Loop health.** `maxLoop` under 20 000 µs throughout, no
`*** loop iteration exceeded ***`, and `maxRead` in the tens of µs. Capture must
not disturb the face: blinking stays smooth while recording.

---

## Playback, streaming and barge-in (Phase 5)

Design and invariants: [docs/PHASE5_PLAN.md](docs/PHASE5_PLAN.md). Still fully
offline — the AI is `LocalMockTurnSource`, behind the same `ITurnSource`
interface the Phase 6 gateway will implement.

```
M5.Mic -> CaptureController -> TurnBuffer (PSRAM) -> TurnStreamer --push--> ITurnSource
                                                        (cursor)          (LocalMockTurnSource)
ITurnSource --pull (peek/consume)--> PcmPlayer (3 DMA slots) -> M5SpeakerOutput -> speaker
                                        |
                                        +-> RMS of the chunk being heard -> cheek bars
                                        +-> first accepted audio -> latency log (no vibration)
```

- **Native-rate playback, no resampler.** M5Unified takes the sample rate per
  `playRaw()` call. Loopback plays the 16 kHz recording at 16 kHz; synthetic
  speech plays at 24 kHz. Every chunk carries an immutable `AudioFormat`; a
  mismatch is rejected, never coerced.
- **Speaker invariant:** `playRaw()` is called only while
  `M5.Speaker.isPlaying(ch) < 2` — otherwise it would spin inside M5Unified
  for a whole chunk. Checked in `PcmPlayer` and again at the only call site.
- **TurnBuffer invariants:** readers see only committed samples, and hold a
  lease that blocks `reset()` until they are done or abandon the turn.
- **Loopback ownership:** `pushUserAudio()` keeps nothing of its borrowed
  pointer. Loopback replays from the TurnBuffer under the mock's *own* lease,
  taken in `beginUserTurn()` (overlapping the streamer's) and released only
  after the player has copied the last sample, or on `cancel()`. It accepts
  only the exact next committed range of that buffer.
- **No callbacks into App.** Control events are a bounded value queue; audio
  is pulled. App alone touches the state machine, face, AudioBus and player.

### New files

```
lib/tth_core/include/tth/  AudioFormat.h  ITurnSource.h  TurnEventQueue.h
                           TurnStreamer.h  PcmPlayer.h  HapticPattern.h
                           LocalMockTurnSource.h  BargeIn.h        (+ src/*.cpp)
src/audio/M5SpeakerOutput.{h,cpp}   the ONLY M5.Speaker.playRaw() call site
src/haptics/Haptics.{h,cpp}         non-blocking refused-action pattern (two short pulses)
test/test_audio_format  test_turn_stream  test_playback
     test_turn_events   test_barge_in     test_haptics
```

### Serial keys (new)

| Key | Effect |
|---|---|
| `m` | mock mode: loopback ⇄ synthetic (from the next response) |
| `p` | simulated backpressure on/off (Busy 150 ms of every 400 ms) |
| `x` | the next response is an injected ERROR |

Face keys `r l w s e a b ?` are unchanged; `s`/`b` still use the simulated
envelope. In production the bars follow real playback amplitude.

---

## Physical test — Phase 5

```powershell
cd d:\tth-bot\tth_bot\firmware\core2
$env:PLATFORMIO_CORE_DIR = "D:\pio-core"
$pio = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
& $pio run -e core2-touch -t upload --upload-port COM3
& $pio device monitor --port COM3 --baud 115200
```

**1. Boot.** Banner `PHASE 5 playback + barge-in (offline)`. Three
`[audio] play slot A/B/C ... DRAM ok` lines (DMA-capable), `synth scratch ...
ok`, `[play] 3 x 960-sample DMA slots (5760 bytes) + 1920-byte synth scratch`,
and `[turn] source: local mock, mode=loopback`. Internal free heap should be
~7.7 KB below Phase 4.

**2. Loopback turn.** Hold, speak ~3 s, release. Expected order:

```
[ptt] PRESS
[capture] start requested: 16000 Hz mono s16le, capacity 1472000 bytes
[mic] recording: 512 samples/chunk at 16000 Hz, 2 queued
[stream] streaming to mock (loopback): 320-sample frames
[app] state ready -> listening (face: listening)
[capture] 500ms samples=... audio=mic ... streamLag=0B
[ptt] RELEASE after ~3000 ms
[capture] stop requested reason=button release
[app] state listening -> waiting (face: waiting)
[stream] DONE samples=S bytes=2S frames=F busyRetries=0 maxLag=...B | mock received=S committed=S gaps=0
[mic] stopped: N chunks delivered, 0 queue stalls
[capture] SUMMARY1 reason=button release ...
[capture] SUMMARY2 ... failed=0 dropped=0 audio=none drainTotal=~61ms
[turn] speech start: 16000 Hz mono s16le (loopback)          <- ~600 ms later
[play] stream open: 16000 Hz mono s16le, 3 x 960-sample slots, audio=speaker
[app] state waiting -> speaking (face: speaking)
[play] first audio accepted by speaker N ms after speech start
[turn] complete: source delivered S samples
[play] SUMMARY end=completed rate=16000Hz queued=Q played=Q samples=S underruns=0 rejects=0 refusals=0 maxLevel=... audio=none drain=...ms
[app] state speaking -> ready (face: ready)
```

Check by ear and eye: **your own voice at its natural pitch** (not fast, not
chipmunked); **one** short buzz as the sound starts; cheek bars rise and fall
with your voice and vanish in pauses; **the smile never changes**. `[stream]`
must show `received == committed` and `gaps=0`.

**3. Synthetic.** Press `m`, then do a short turn. Expect `speech start: 24000
Hz`, `stream open: 24000 Hz`, and a 12 s voice-like tone in three phrases
(quiet, medium, loud) with silences. The bars show three distinct heights and
disappear between phrases. `[play] SUMMARY ... rate=24000Hz samples=288000
underruns=0`. Press `m` again to return to loopback.

**4. Backpressure.** Press `p` (ON), do a ~5 s turn, press `p` again. Expect
`[stream] DONE ... busyRetries>0 maxLag>0 | mock received=S committed=S gaps=0`
— Busy cost retries, never samples — and the loopback plays the whole turn.

**5. Barge-in.** In synthetic mode, press and hold while it speaks. Sound must
stop at once, then:

```
[ptt] PRESS
[barge] press while speaker active (player streaming): stopping playback first
[play] SUMMARY end=cancelled rate=24000Hz ... audio=none drain=...ms
[capture] start requested: ...
[mic] recording: ...
[stream] streaming to mock (synthetic): 320-sample frames
[barge] listening: speaker quiet after N ms, press->mic N us
[app] state speaking -> listening (face: listening)
```

The face must not show LISTENING before those lines. Speak, release: a normal
turn follows. Also **tap** briefly during speech: expect `[barge] released
early` and `speaking -> ready`, with no capture started.

**6. Error.** Press `x`, do a turn. After the think delay: `[turn] ERROR from
source: injected`, `waiting -> error`, dim ERROR face. The next press starts a
normal turn.

**7. 45 s loopback.** Hold for the full 45 s. The forced release is followed
by 45 s of playback at 16 kHz: `underruns=0`, `samples` ≈ 720 000.

**8. Health (every heartbeat).** `[hb]` `maxLoopSteady` ≈ 13 ms with no
overrun warning; `[pb] ... underruns=0 rejects=0 guard=0/0 maxLoopSpeaking=...
evDrop=0`; `[mem]` flat across ten or more turns.

**Please report these measurements** (they are deliberately not assumed):

| Figure | Where |
|---|---|
| `M5.Speaker.begin`, `spk.playRaw#1`, `spk.playRaw` | `[blocks]` — the predicted speaker priming cost |
| `M5.Speaker.stop`, `M5.Speaker.end` | `[blocks]` |
| barge-in `press->mic`, and `barge.*` stages | `[barge]`, `[blocks]` |
| `maxLoopSpeaking` | `[pb]` |
| underruns over the 45 s turn | `[play] SUMMARY` |
| internal free / largest block during playback | `[mem]` |
| bars vs sound: do they lead, lag, or look in step? | by eye |
| loudness at `TTH_SPEAKER_VOLUME 255` (maximum); bar height at `TTH_BAR_LEVEL_GAIN 3.0` | by ear / eye — both tunable in `Config.h` |

---

## Loopback loudness

Physical testing found recorded-voice loopback noticeably quiet while
synthetic speech was fine. The signal path, as configured (printed at boot):

| Stage | Setting | Effect |
|---|---|---|
| Mic (SPM1423 PDM) | 16 kHz, `magnification=16`, `over_sampling=1`, `noise_filter=0` (M5Unified generic internal-mic setup; Core2 only sets pins) | fixed by M5Unified |
| Capture | DC removal only | no gain |
| Stream / TurnBuffer | none | bit-exact |
| **Loopback gain (new)** | `TTH_LOOPBACK_GAIN_DB` | loopback only |
| PcmPlayer | copies unchanged | `inputPeak` now measured |
| M5 speaker mixer | `magnification=16`, master `96` at that test (now `255`), channel `255` | **×0.140 (−17.1 dB)** at 96; ×0.984 (−0.1 dB) at 255 |

**M5Unified squares the volume** (`volume = magnification × master²`, times
`channel²`): amplitude = 16·V²·255²/2³⁶. V = 96 → −17.1 dB, V = 160 → −8.2 dB,
V = 255 → ≈0 dB. So the step from 160 to 96 cost 8.9 dB, and the mixer never
amplifies. Synthetic speech is generated at about −5 dBFS; a voice recording
sits well below that. The cure is a gain on the recording, not the master
volume, which would raise synthetic and future Gemini speech too. (The A/B
then showed the gained recording reaching ~0.9 of full scale, so the master
volume was raised — see "Speaker volume"; it is now 255.)
Above ~160 the limits are analog (the 1 W speaker and the
NS4168 near full swing rattle; full-scale bursts draw current peaks on battery).

**The gain** (`tth/PcmGain.h`, applied only inside `LocalMockTurnSource`'s
loopback branch):
- one gain per turn: the configured gain, reduced only if it would push the
  turn's 99.9th-percentile level above −3 dBFS, never below unity;
- signed 32-bit arithmetic, gain capped at 18 dB so no product can wrap, then
  saturation to int16;
- a memoryless soft-knee limiter above −3 dBFS that approaches but never
  reaches full scale — clicks and plosives round off, not clip;
- no per-chunk normalisation, so nothing pumps between words; the output is
  identical however the turn is chunked;
- **0 dB is an exact bypass** (the recording, zero-copy, bit for bit).

Per-turn log: `[loopback] turn level: peak=… p99.9=… -> gain +9.0 dB configured,
+9.0 dB applied`, and `[play] SUMMARY2 source=… gainDb=… inputPeak=… outputPeak=…
limitedSamples=… maxLevel=…`. Key **`g`** cycles 0 → 6 → 9 → 12 dB (from the next
user turn).

### A/B test — choose the final loopback gain

```powershell
cd d:\tth-bot\tth_bot\firmware\core2
$env:PLATFORMIO_CORE_DIR = "D:\pio-core"
$pio = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
& $pio run -e core2-touch -t upload --upload-port COM3
& $pio device monitor --port COM3 --baud 115200
```

**Boot:** confirm `[audio] mic: 16000 Hz, magnification=16, over_sampling=1,
noise_filter=0`, `[audio] speaker: ... master=96, channel=255 -> path gain 0.140
(-17.1 dB of full scale)`, and `[audio] loopback gain: +9.0 dB configured`.

**Keep everything constant:** same seat, device ~30 cm away on the table, same
normal speaking voice, same phrase every time — e.g. *"One, two, three. The
little robot is listening."* Loopback mode (the default).

Boot starts at +9 dB; each `g` advances 9 → 12 → 0 → 6 → 9. Run the four in
this order, pressing `g` once before each and checking the log line
`[loopback] gain: +N.N dB (from the next user turn)`:

| Step | Press `g` until | Then |
|---|---|---|
| A | `+0.0 dB` | say the phrase — the untouched recording (reference) |
| B | `+6.0 dB` | say the phrase |
| C | `+9.0 dB` | say the phrase |
| D | `+12.0 dB` | say the phrase |

For each step note the two lines:

```
[loopback] turn level: peak=0.xxx p99.9=0.xxx -> gain +N.N dB configured, +N.N dB applied
[play] SUMMARY2 source=loopback gainDb=+N.N inputPeak=0.xxx outputPeak=0.xxx limitedSamples=N maxLevel=0.xxx
```

Expected: at 0 dB, `outputPeak == inputPeak` and `limitedSamples=0`; at higher
gains `outputPeak ≈ inputPeak × 10^(gain/20)` (×2.00 at 6, ×2.82 at 9, ×3.98 at
12) unless `applied` is lower than `configured` — that only happens on a hot
recording, and then applies to the whole turn. `limitedSamples` should stay a
tiny fraction of the samples.

Then, at whichever gain sounds best, two stress checks:
1. **Loud:** say the phrase loudly, 10 cm from the device. `applied` may drop
   below `configured`; the playback must sound loud but **not harsh or
   crackly**.
2. **Quiet room pauses:** say two words with a 2 s pause. The background hiss
   in the pause is raised by the gain like everything else, but it must **not
   swell or pump** between words.

Also compare against synthetic (`m`, one turn, `m` again): the chosen loopback
gain should sound roughly as loud as the synthetic voice. The cheek bars follow
what is heard, so they grow with the gain — check they are not pinned at the
top.

**Report back:** your chosen gain, and the two log lines from each of A–D. The
choice becomes `TTH_LOOPBACK_GAIN_DB` in `Config.h`.

---

## Speaker volume

The loopback-gain A/B showed the gained recording already reaching ~0.9 of
full scale (`outputPeak` 0.88–0.93), so the remaining attenuation was
M5Unified's master volume, applied **squared**. It is now the maximum
M5Unified supports:

**`TTH_SPEAKER_VOLUME 255`** → path gain ×0.984 (−0.1 dB): full scale in is
(almost) full scale out, for loopback and synthetic playback alike.

| Volume | Path gain | Attenuation |
|---:|---:|---:|
| 96 (first Phase 5 test) | ×0.140 | −17.1 dB |
| 160 | ×0.388 | −8.2 dB |
| **255** (now) | ×0.984 | −0.1 dB |

- The mixer still cannot clip digitally (its gain stays below 1). The limits
  are analog: listen for rattle or buzz on loud passages, and watch for
  resets on battery.
- The +9 dB loopback gain and its soft limiter are unchanged.
- The 96–220 runtime volume cycle (`v`) was removed: with the level fixed at
  the maximum there is nothing left to step through. To change the level,
  edit `TTH_SPEAKER_VOLUME` in `Config.h` (0..255).

Where it shows in the log:

```
[audio] speaker: 48000 Hz out, magnification=16, master=255, channel=255 -> path gain 0.984 (-0.1 dB of full scale)
[play] stream open: 16000 Hz mono s16le, 3 x 960-sample slots, audio=speaker master=255 (path -0.1 dB)
[play] SUMMARY2 source=loopback gainDb=+N.N inputPeak=0.xxx outputPeak=0.xxx limitedSamples=N maxLevel=0.xxx master=255 pathGainDb=-0.1
```

### Quick physical check

```powershell
cd d:\tth-bot\tth_bot\firmware\core2
$env:PLATFORMIO_CORE_DIR = "D:\pio-core"
$pio = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
& $pio run -e core2-touch -t upload --upload-port COM3
& $pio device monitor --port COM3 --baud 115200
```

1. Boot: the `[audio] speaker: ... master=255 ... path gain 0.984` line.
2. One loopback turn at normal voice and one loud, 10 cm away: every
   `SUMMARY2` ends `master=255 pathGainDb=-0.1`; listen for rattle or
   distortion that is not in your voice.
3. One synthetic turn (`m`, speak, `m` again): its loud third phrase is the
   hottest signal the speaker gets — it must not rattle.
4. Unplug USB and do one synthetic turn on battery while watching the face:
   it must not reset.

---

## Power supply and audible whine (hardware observation)

Observed during the Phase 5 physical test: an audible whine from the speaker
**only while USB power is connected**. It disappears completely on the
internal battery. It is USB power-path noise reaching the speaker amplifier,
not a playback defect: the firmware produces the same audio either way, and
playback reports `underruns=0 rejects=0 refusals=0` in both.

- **Recommended use: battery-powered during conversation.**
- **Charge while idle**, not while talking to the robot.
- **If powered operation is required**, test a quality wall adapter or power
  bank rather than a computer USB port, which is the usual source of this
  kind of ground and switching noise.

No DSP filtering is applied for this, deliberately. The noise originates in
the power path, not in the PCM, so a filter on the audio would not remove it
and would only colour the speech.
