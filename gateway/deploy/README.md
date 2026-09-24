# TTH gateway on the Raspberry Pi (LAN host)

Runbook for running the gateway unattended on the Raspberry Pi. The robot
reaches it at `wss://192.168.1.150:8443/v1/ws`.

| | |
|---|---|
| Host | Raspberry Pi 4 Model B 8 GB, Raspberry Pi OS Lite 64-bit (Debian 13, arm64) |
| Host name / admin | `tth-gateway` / `tthadmin` |
| Address | `192.168.1.150` (reserved in the router's DHCP) |
| Runtime | Deno **2.9.6** (native, no Docker), managed by `systemd` |
| Mode | `GEMINI_MODE=live`: tokens minted through `gateway-gemini-token` |

Commands marked **Windows** run on the development PC. Commands marked
**Pi** run over SSH as `tthadmin`. No step prints a secret. Never paste a
secret into a command line.

## Layout

```
/usr/local/lib/deno/2.9.6/deno        pinned Deno (install_deno.sh)
/usr/local/bin/deno -> ../lib/deno/2.9.6/deno
/opt/tth-gateway/                     root 0755
  repo/                               clone of github.com/angheltofan/TTH-Bot
  releases/<sha>/                     one detached worktree per tested commit
  current  -> releases/<sha>          what the service runs
  previous -> releases/<sha>          what `rollback` returns to
  deploy.log                          deploy / rollback history (no secrets)
/etc/tth-gateway/                     root 0700
  gateway.env                         0600  byte-identical copy of the Windows gateway/.env
  tls/gateway.key                     0600  Pi gateway private key
  tls/gateway.pem                     0644  Pi gateway certificate
  tls/ca.pem                          0644  the existing dev CA (public), for health checks
/var/backups/tth-gateway/             root 0700  previous secrets and unit files
/var/cache/tth-gateway/               Deno cache of the service (systemd CacheDirectory)
/etc/systemd/system/tth-gateway.service
/etc/systemd/journald.conf.d/tth-gateway.conf
```

The CA private key `ca.key` **never** goes to the Pi. It stays on Windows,
in `gateway\.dev-certs\` and in the offline backup. `pi_release.sh`
refuses to run when it finds a CA key file on the Pi.

## How the service runs

[`tth-gateway.service`](tth-gateway.service):

- **Secrets.** `systemd` reads the three secret files as root and passes
  them to the process as *credentials*, in a private read-only directory
  (`%d`). The service runs as a throw-away user (`DynamicUser`) that cannot
  read `/etc/tth-gateway`. Deno may read only that credentials directory
  (`--allow-read=%d`).
- **Settings the unit overrides.** The `.env` from Windows still says
  `GEMINI_MODE=fake` and has Windows TLS paths. Deno's `--env-file` never
  overrides a variable that is already set, so the unit's
  `GEMINI_MODE=live`, `PORT=8443`, `TLS_CERT_FILE` and `TLS_KEY_FILE` take
  effect. The file itself is never edited.
- **Clock first.** It starts only after NTP has synchronised
  (`time-sync.target` with `systemd-time-wait-sync`). The Pi has no
  battery-backed clock, the token mint rejects timestamps more than 60 s off,
  and TLS needs the correct date. **Without internet at boot, the gateway
  waits** (it would be useless without internet anyway).
- **Restart.** `Restart=always`, with a back-off from 5 s up to 5 min, and it
  never gives up.
- **Stop.** A stop closes robot connections abruptly (no SIGTERM handler, by
  decision). The robot reconnects, exactly as after Ctrl+C on Windows. Restart
  only while no one is talking to the robot.
- **Hardening.** `ProtectSystem=strict`, no capabilities, `@system-service`
  system calls, `MemoryMax=1G`. `MemoryDenyWriteExecute` is deliberately
  absent because V8's JIT needs it.
- **Health.** `GET /healthz` answers `ok`. This is **liveness only**: it does
  not check Supabase, the mint or Gemini. The end-to-end proof is the robot
  reaching READY and completing a turn.

## 1. Before anything: back up (Windows)

Back up the Windows secrets **outside any Git repository**. Afterwards, move
the copy to offline, encrypted storage (a BitLocker USB stick or a password
manager's file vault).

```powershell
$g  = "D:\tth-bot\tth_bot\gateway"
$bk = "D:\tth-bot-secrets-backup\$(Get-Date -Format yyyyMMdd-HHmmss)"
New-Item -ItemType Directory $bk | Out-Null
Copy-Item "$g\.env" $bk
Copy-Item -Recurse "$g\.dev-certs" $bk
Get-ChildItem -Recurse $bk | Select-Object FullName, Length
```

Expected: `.env`, `ca.key`, `ca.pem`, `gateway.key`, `gateway.pem`. This is
the **only** recovery copy of the CA. If you lose `ca.key`, you must create a
new CA and provision it onto every robot again.

## 2. Pi certificate from the existing CA (Windows, Git Bash)

`make_dev_certs.sh` always **overwrites** the gateway certificate and key in
its output folder. It **creates a new CA** when that folder has none. So give
it a separate folder that already holds a copy of the existing CA. The
original CA and the current `192.168.1.134` certificate stay untouched, and
the robot's pinned CA stays valid.

```bash
cd /d/tth-bot/tth_bot/gateway
git check-ignore -q .dev-certs/pi-150/gateway.key && echo "ignored by Git"   # must print it
test ! -e .dev-certs/pi-150 || echo "STOP: .dev-certs/pi-150 already exists"
mkdir .dev-certs/pi-150
cp -p .dev-certs/ca.pem .dev-certs/ca.key .dev-certs/pi-150/
TTH_DEV_CERT_DIR=.dev-certs/pi-150 bash scripts/make_dev_certs.sh 192.168.1.150 tth-gateway tth-gateway.local
rm -f .dev-certs/pi-150/ca.key                # the COPY; the original stays in .dev-certs/
cmp .dev-certs/ca.pem .dev-certs/pi-150/ca.pem && echo "same CA"
openssl verify -CAfile .dev-certs/ca.pem .dev-certs/pi-150/gateway.pem
openssl x509 -in .dev-certs/pi-150/gateway.pem -noout -subject -dates -ext subjectAltName
```

**Expected:**
- The script prints `reusing the existing dev CA`. If it does not, stop:
  delete `pi-150` and start again.
- `same CA` and `gateway.pem: OK` are printed.
- The subjectAltName list is `IP Address:192.168.1.150, DNS:192.168.1.150,
  DNS:tth-gateway, DNS:tth-gateway.local`. The robot's mbedTLS matches DNS
  names only, which is why the IP is also listed as a DNS name.
- The certificate is valid for 825 days. `preflight` refuses a certificate
  that has 30 days or less left. Renew it by repeating this step in a new
  folder.

## 3. Host preparation (Pi, once)

Flash Raspberry Pi OS Lite 64-bit with host name `tth-gateway`, user
`tthadmin` and SSH enabled, and reserve `192.168.1.150` for it in the
router. Then:

```bash
sudo apt-get update && sudo apt-get full-upgrade -y
sudo apt-get install -y git curl unzip openssl python3 ca-certificates
timedatectl                                   # "System clock synchronized: yes"
```

Bootstrap with the scripts of the **pinned commit** (`<sha>` = the full
40-character commit that contains `gateway/deploy/`):

```bash
git clone https://github.com/angheltofan/TTH-Bot.git ~/tth-bootstrap
git -C ~/tth-bootstrap checkout --detach <sha>
sudo bash ~/tth-bootstrap/gateway/deploy/install_deno.sh
sudo bash ~/tth-bootstrap/gateway/deploy/pi_release.sh setup
```

- `install_deno.sh` downloads Deno 2.9.6 for arm64 from the official
  release. It installs it only if the archive's SHA-256 equals **both** the
  release's published `.sha256sum` and GitHub's own digest for that asset. No
  checksum is written into the script.
- `setup` is idempotent. It creates the directories and clones the
  repository. It enables a persistent journal (`SystemMaxUse=200M`,
  `SystemKeepFree=1G`, `SystemMaxFileSize=25M`, `MaxRetentionSec=3month`;
  these limits apply to the whole system journal) and enables
  `systemd-time-wait-sync`. It does not touch secrets or the service.

## 4. Transfer the secrets (Windows → Pi)

Copy files as files: nothing is displayed, typed or regenerated. `ca.key` is
**not** in the list.

**Windows (PowerShell):**

```powershell
$g = "D:\tth-bot\tth_bot\gateway"
ssh tthadmin@192.168.1.150 "install -d -m 700 ~/tth-staging"
scp "$g\.env" tthadmin@192.168.1.150:tth-staging/gateway.env
scp "$g\.dev-certs\pi-150\gateway.pem" "$g\.dev-certs\pi-150\gateway.key" "$g\.dev-certs\ca.pem" tthadmin@192.168.1.150:tth-staging/
Get-FileHash "$g\.env", "$g\.dev-certs\pi-150\gateway.pem", "$g\.dev-certs\pi-150\gateway.key", "$g\.dev-certs\ca.pem" | Format-Table Hash, Path
```

**Pi:** check that the hashes match what Windows printed (they are hashes,
not the contents). Then back up whatever is already installed, and install:

```bash
sha256sum ~/tth-staging/*
sudo sh -c 'd=/var/backups/tth-gateway/secrets-$(date -u +%Y%m%dT%H%M%SZ); install -d -m 0700 "$d"; cp -a /etc/tth-gateway/. "$d/"'
sudo install -o root -g root -m 0600 ~/tth-staging/gateway.env /etc/tth-gateway/gateway.env
sudo install -o root -g root -m 0600 ~/tth-staging/gateway.key /etc/tth-gateway/tls/gateway.key
sudo install -o root -g root -m 0644 ~/tth-staging/gateway.pem /etc/tth-gateway/tls/gateway.pem
sudo install -o root -g root -m 0644 ~/tth-staging/ca.pem      /etc/tth-gateway/tls/ca.pem
shred -u ~/tth-staging/* && rmdir ~/tth-staging
sudo bash ~/tth-bootstrap/gateway/deploy/pi_release.sh preflight
```

- `GATEWAY_MINT_SECRET` travels inside `gateway.env` with the same value, so
  nothing changes in Supabase. `TTH_DEVICES` (the device registry) travels
  the same way, so the robot keeps its token.
- `shred` is best effort on flash storage. The staging copies only lived in
  the admin's 0700 directory.
- `preflight` checks without printing any value:
  - the files' ownership and modes;
  - that the four required settings have values (checked by name);
  - that there is no `FAKE_*` setting and no `TRUST_PROXY=1`;
  - that there is no CA key on the Pi;
  - the certificate chain, its expiry and its names;
  - that the key matches the certificate (by public-key fingerprint);
  - NTP sync and the Deno version.

## 5. Deploy the pinned commit (Pi)

```bash
sudo bash ~/tth-bootstrap/gateway/deploy/pi_release.sh deploy <sha> [--branch <branch>]
sudo bash /opt/tth-gateway/current/gateway/deploy/pi_release.sh status
rm -rf ~/tth-bootstrap                         # later deploys use /opt/tth-gateway/current/...
```

The commit must be on `origin/main`, or on the branch named with `--branch`
(for example `--branch raspberry-pi-gateway` before it is merged).

`deploy` does, and stops at the first failure:

1. Runs `preflight`, then fetches and checks that the commit is on that branch.
2. Creates `releases/<sha>` as a clean detached worktree.
3. Runs `systemd-analyze verify` on the release's unit file.
4. Runs `deno task check` and `deno task test` in a throw-away `systemd-run`
   sandbox with no network, no secrets and a read-only file system. A release
   that fails is deleted.
5. Installs that release's unit (the old unit is backed up to
   `/var/backups/tth-gateway/units/`), then enables the service.
6. Switches `current` atomically, then restarts the service.
7. Waits up to 45 s for `https://tth-gateway:8443/healthz` = `ok`, over TLS
   verified against `ca.pem` and connected to `127.0.0.1`. Then it checks that
   the process has not restarted within 10 s.
8. **On failure**, it goes back to the release that was running, reinstalls
   that release's unit, and checks its health again. If there was no earlier
   release, it stops the service.
9. Keeps the three newest releases, plus `current` and `previous`.

Deploying the commit that is already active and healthy does nothing. The
script refuses to run twice at the same time (`flock`).

**Windows check:**

```powershell
curl.exe --ssl-no-revoke --cacert D:\tth-bot\tth_bot\gateway\.dev-certs\ca.pem https://192.168.1.150:8443/healthz
```

Expected: `ok`. With Avast's HTTPS scanning, add an exception as described in
`firmware/core2/README.md`.

**Reboot test:** `sudo reboot`. Do not log in again; after about a minute the
Windows check answers `ok` again.

## 6. Point the robot at the Pi (Windows, USB, once)

1. Stop any gateway on Windows (Ctrl+C in its window). Two live gateways must
   not share the robot.
2. Close the serial monitor.
3. Run:

   ```powershell
   $py = "$env:USERPROFILE\.platformio\penv\Scripts\python.exe"
   & $py firmware\core2\tools\provision.py --port COM3 --set-url "wss://192.168.1.150:8443/v1/ws"
   ```

`--set-url` keeps the Wi-Fi credentials, the device token and the CA, so no
new registry entry is needed and no CA has to be provisioned: the Pi's
certificate comes from the same CA.

**Expected:**
- The robot logs `[config] stored url: configured (wss host=192.168.1.150
  port=8443 path=/v1/ws)`, then `[gw] READY`.
- A push-to-talk turn is answered.
- On the Pi, `journalctl -u tth-gateway -f` shows the session events (device
  id, activity UUID and counters only).

To return the robot to the Windows gateway, run `--set-url` again with the
PC's address.

## 7. Everyday operations (Pi)

```bash
R=/opt/tth-gateway/current/gateway/deploy/pi_release.sh
sudo bash $R status                         # current/previous, releases, service, health, last deploys
sudo bash $R deploy <new sha> [--branch <b>]  # update to a pinned commit
sudo bash $R rollback                       # back to `previous` (health-checked; swaps current/previous)
journalctl -u tth-gateway -f                # live logs
journalctl -u tth-gateway --since today
journalctl --disk-usage
cat /opt/tth-gateway/deploy.log
```

- Update only while the robot is idle: a restart drops its session.
- Secrets live outside the releases, so deploy and rollback never touch them.
  To replace a secret, repeat step 4 (it backs up first), then run
  `sudo systemctl restart tth-gateway`.
- **Deno upgrade.** Change `VERSION` in `install_deno.sh` **and**
  `DENO_VERSION` in `pi_release.sh` in the same commit. Run the new
  `install_deno.sh` (the old version stays installed side by side), then
  deploy that commit. To go back, run the old commit's `install_deno.sh`,
  which only switches the symlink, then roll back.
- **Unit changes** ship with the commit: `deploy` and `rollback` always
  install the unit of the release they activate.

## 8. LAN firewall (documented; activate only after steps 5–6 pass)

Raspberry Pi OS Lite has no firewall enabled. The gateway only needs TCP
8443 from the LAN. SSH needs TCP 22.

`/etc/nftables.conf`:

```
#!/usr/sbin/nft -f
flush ruleset

table inet filter {
  chain input {
    type filter hook input priority filter; policy drop;
    iif "lo" accept
    ct state established,related accept
    ct state invalid drop
    meta l4proto { icmp, ipv6-icmp } accept
    ip saddr 192.168.1.0/24 tcp dport { 22, 8443 } accept
    udp sport 67 udp dport 68 accept                           # DHCP (IPv4 lease)
    ip saddr 192.168.1.0/24 udp dport 5353 accept              # mDNS (tth-gateway.local)
  }
  chain forward {
    type filter hook forward priority filter; policy drop;
  }
  chain output {
    type filter hook output priority filter; policy accept;
  }
}
```

Activate it with a dead-man switch. If SSH breaks, the rules are removed
automatically after 5 minutes:

```bash
sudo apt-get install -y nftables
sudo nft -c -f /etc/nftables.conf                              # syntax check only
sudo systemd-run --unit=tth-fw-revert --on-active=5min /usr/sbin/nft flush ruleset
sudo nft -f /etc/nftables.conf
# In a NEW terminal: ssh tthadmin@192.168.1.150 works, and the Windows /healthz check answers ok.
sudo systemctl stop tth-fw-revert.timer                        # keep the rules
sudo systemctl enable nftables                                 # load them at boot
```

## Troubleshooting

| Symptom | Where to look |
|---|---|
| `preflight`: clock not NTP-synchronised | `timedatectl`, `systemctl status systemd-timesyncd`; internet reachable? |
| `preflight`: CA key present | Delete it from the Pi; `ca.key` stays on Windows only |
| `preflight`: certificate lacks a name or expires soon | Repeat step 2 (new folder), then step 4 |
| `deploy`: not on `origin/main` | Push/merge first, or pass `--branch` |
| Health check fails | `journalctl -u tth-gateway -n 50 --no-pager`: `missing required setting …`, `GATEWAY_MINT_SECRET is too short`, TLS file errors |
| Service `activating (auto-restart)` | Crash loop with back-off; same journal |
| Robot: `network or timeout` | Pi reachable? `/healthz` from Windows; firewall (step 8) |
| Robot: TLS / certificate error | Is the Pi certificate from the same CA as the robot's `ca.pem`? (step 2, `same CA`) |
| Robot: HTTP 401 | `TTH_DEVICES` in `gateway.env` differs from the one the robot was provisioned with |
| Robot: HTTP 503 before READY | Supabase activity unavailable (see gateway/README.md, "Fail closed before Gemini") |

**SD card lost or replaced:** repeat steps 3–5 with the same pinned commit.
The secrets come again from Windows or from the backup in step 1. Nothing
needs to be regenerated.
