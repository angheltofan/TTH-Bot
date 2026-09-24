#!/usr/bin/env bash
# TTH gateway releases on the Raspberry Pi LAN host (gateway/deploy/README.md).
#
#   sudo bash pi_release.sh setup                     host preparation (idempotent)
#   sudo bash pi_release.sh preflight                 secrets, certificate, clock, Deno
#   sudo bash pi_release.sh deploy <40-hex sha> [--branch <name>]
#   sudo bash pi_release.sh rollback                  back to the previous release
#   sudo bash pi_release.sh status
#
# deploy: fetch -> the commit must be on origin/<branch> (default main) ->
# detached worktree releases/<sha> -> `deno task check` + `deno task test` in
# a throw-away sandbox (no network, no secrets) -> install the release's unit
# -> switch `current` atomically -> restart -> /healthz over TLS, then a
# stability check -> on failure, automatic return to the previous release.
# The last three releases are kept (never `current` or `previous`).
#
# Fails closed, and never prints a secret: secret files are inspected only
# for existence, ownership, mode and variable NAMES; the gateway key only
# through a public-key fingerprint comparison.
set -euo pipefail
umask 022

ROOT=/opt/tth-gateway
REPO="$ROOT/repo"
RELEASES="$ROOT/releases"
DEPLOY_LOG="$ROOT/deploy.log"
ETC=/etc/tth-gateway
BACKUPS=/var/backups/tth-gateway
REMOTE=https://github.com/angheltofan/TTH-Bot.git
SERVICE=tth-gateway.service
UNIT_DST="/etc/systemd/system/$SERVICE"
JOURNALD_DROPIN=/etc/systemd/journald.conf.d/tth-gateway.conf
DENO=/usr/local/bin/deno
DENO_VERSION=2.9.6
PORT=8443
LAN_IP=192.168.1.150
CERT_NAMES=("IP Address:$LAN_IP" "DNS:$LAN_IP" "DNS:tth-gateway" "DNS:tth-gateway.local")
HEALTH_URL="https://tth-gateway:$PORT/healthz"
HEALTH_TIMEOUT_S=45
STABLE_S=10
KEEP=3
LOCK=/run/lock/tth-gateway-release.lock

say() { echo "pi_release: $*"; }
fail() { echo "pi_release: ERROR: $*" >&2; exit 1; }
need_root() { [ "$(id -u)" -eq 0 ] || fail "run as root (sudo)"; }
need_tools() {
  local t
  for t in "$@"; do command -v "$t" >/dev/null 2>&1 || fail "missing tool: $t"; done
}
log_line() { printf '%s %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*" >>"$DEPLOY_LOG"; }
lock() {
  exec 9>"$LOCK"
  flock -n 9 || fail "another pi_release.sh is running"
}

# ---------------------------------------------------------------- setup ----

write_if_changed() { # <path> <mode> ; content on stdin. Returns 0 if changed.
  local tmp
  tmp="$(mktemp)"
  cat >"$tmp"
  if [ -f "$1" ] && cmp -s "$tmp" "$1"; then
    rm -f "$tmp"
    return 1
  fi
  install -D -o root -g root -m "$2" "$tmp" "$1"
  rm -f "$tmp"
  return 0
}

cmd_setup() {
  need_root
  need_tools git curl openssl flock systemd-run systemd-analyze timedatectl journalctl cmp
  lock

  install -d -o root -g root -m 0755 "$ROOT" "$RELEASES"
  install -d -o root -g root -m 0700 "$ETC" "$ETC/tls" "$BACKUPS"
  [ -f "$DEPLOY_LOG" ] || install -o root -g root -m 0644 /dev/null "$DEPLOY_LOG"
  say "directories ready ($ROOT, $ETC, $BACKUPS)"

  if [ -e "$REPO" ]; then
    [ "$(git -C "$REPO" remote get-url origin)" = "$REMOTE" ] || fail "$REPO is not a clone of $REMOTE"
    say "repository present ($REPO)"
  else
    git clone --quiet --no-checkout "$REMOTE" "$REPO"
    say "cloned $REMOTE into $REPO"
  fi

  # Persistent journal with a bounded footprint on the SD card. These limits
  # apply to the whole system journal, not only to the gateway.
  if write_if_changed "$JOURNALD_DROPIN" 0644 <<'EOF'
# TTH gateway host (gateway/deploy/README.md): persistent, bounded journal.
[Journal]
Storage=persistent
SystemMaxUse=200M
SystemKeepFree=1G
SystemMaxFileSize=25M
MaxRetentionSec=3month
EOF
  then
    systemctl restart systemd-journald
    journalctl --flush
    say "journald: persistent, SystemMaxUse=200M, MaxRetentionSec=3month"
  else
    say "journald drop-in unchanged"
  fi

  # time-sync.target only waits for NTP while this service is enabled.
  systemctl enable --quiet systemd-time-wait-sync.service
  systemctl is-enabled --quiet systemd-timesyncd.service ||
    say "WARNING: systemd-timesyncd is not enabled - enable an NTP client"
  say "systemd-time-wait-sync enabled"

  if deno_ok; then say "Deno $DENO_VERSION present"; else say "Deno $DENO_VERSION missing: run install_deno.sh"; fi
  say "setup complete (secrets, unit and service are NOT touched by setup)"
}

# ------------------------------------------------------------ preflight ----

deno_ok() { [ -x "$DENO" ] && "$DENO" --version 2>/dev/null | head -n1 | grep -q "^deno $DENO_VERSION "; }

check_secret_file() { # <path> <allowed modes regex>
  [ -f "$1" ] && [ ! -L "$1" ] || fail "missing (or a symlink): $1"
  [ -s "$1" ] || fail "empty: $1"
  [ "$(stat -c %U:%G "$1")" = "root:root" ] || fail "must be owned by root:root: $1"
  stat -c %a "$1" | grep -Eqx "$2" || fail "wrong mode $(stat -c %a "$1") (expected $2): $1"
}

env_has_value() { # <NAME> - true when the env file assigns a non-empty value
  grep -Eq "^[[:space:]]*(export[[:space:]]+)?$1[[:space:]]*=[[:space:]]*[^[:space:]#]" "$ETC/gateway.env"
}

cmd_preflight() {
  need_root
  need_tools openssl stat grep timedatectl sha256sum
  local f found name san names missing pub_key pub_cert

  [ -d "$ETC" ] && [ "$(stat -c %U:%a "$ETC")" = "root:700" ] || fail "$ETC must exist, root-owned, mode 700"
  check_secret_file "$ETC/gateway.env" '600'
  check_secret_file "$ETC/tls/gateway.key" '600'
  check_secret_file "$ETC/tls/gateway.pem" '6[04][04]'
  check_secret_file "$ETC/tls/ca.pem" '6[04][04]'

  # The CA private key never belongs on the Pi.
  found="$(find "$ETC" "$ROOT" "$BACKUPS" -xdev \( -iname 'ca.key' -o -iname 'ca*.key' -o -iname '*ca*key*.pem' \) -print -quit 2>/dev/null || true)"
  [ -z "$found" ] || fail "a CA key file is present ($found) - remove it; ca.key stays on Windows"
  while IFS= read -r -d '' f; do
    [ "$f" = "$ETC/tls/gateway.key" ] && continue
    grep -qs -- 'PRIVATE KEY-----' "$f" && fail "unexpected private key material in $f"
  done < <(find "$ETC" -type f -print0)

  # Required settings, by NAME only (values are never read into the output).
  missing=""
  for name in SUPABASE_URL SUPABASE_PUBLISHABLE_KEY GATEWAY_MINT_SECRET TTH_DEVICES; do
    env_has_value "$name" || missing="$missing $name"
  done
  [ -z "$missing" ] || fail "gateway.env lacks a value for:$missing"
  names="$(sed -nE 's/^[[:space:]]*(export[[:space:]]+)?(FAKE_[A-Z_]+)[[:space:]]*=[[:space:]]*[^[:space:]#].*/\2/p' "$ETC/gateway.env" | tr '\n' ' ')"
  [ -z "$names" ] || fail "gateway.env sets fake-mode settings refused in live mode: $names"
  grep -Eq '^[[:space:]]*(export[[:space:]]+)?TRUST_PROXY[[:space:]]*=[[:space:]]*["'"'"']?1' "$ETC/gateway.env" &&
    fail "TRUST_PROXY=1 in gateway.env: the Pi is reached directly, not through a proxy"

  # Certificate (public): chain, validity, names; key match by fingerprint.
  openssl verify -CAfile "$ETC/tls/ca.pem" "$ETC/tls/gateway.pem" >/dev/null 2>&1 ||
    fail "tls/gateway.pem does not verify against tls/ca.pem"
  openssl x509 -in "$ETC/tls/gateway.pem" -noout -checkend 2592000 >/dev/null ||
    fail "tls/gateway.pem expires within 30 days (or has expired)"
  san="$(openssl x509 -in "$ETC/tls/gateway.pem" -noout -ext subjectAltName 2>/dev/null)"
  for name in "${CERT_NAMES[@]}"; do
    grep -Eq "(^|[ ,])$name(,|$)" <<<"$san" || fail "tls/gateway.pem lacks subjectAltName $name"
  done
  pub_key="$(openssl pkey -in "$ETC/tls/gateway.key" -pubout 2>/dev/null | sha256sum)" ||
    fail "tls/gateway.key is not a readable private key"
  pub_cert="$(openssl x509 -in "$ETC/tls/gateway.pem" -noout -pubkey | sha256sum)"
  [ "$pub_key" = "$pub_cert" ] || fail "tls/gateway.key does not match tls/gateway.pem"

  [ "$(timedatectl show -p NTPSynchronized --value)" = "yes" ] ||
    fail "the clock is not NTP-synchronised (the token mint allows 60 s of skew)"
  deno_ok || fail "Deno $DENO_VERSION is not installed at $DENO (run install_deno.sh)"
  say "preflight passed (secrets present with safe modes, certificate valid for ${CERT_NAMES[*]}, clock synced, Deno $DENO_VERSION)"
}

# -------------------------------------------------------------- releases ----

current_sha() { local t; t="$(readlink "$ROOT/current" 2>/dev/null)" || return 0; basename "$t"; }
previous_sha() { local t; t="$(readlink "$ROOT/previous" 2>/dev/null)" || return 0; basename "$t"; }

set_link() { # <name> <sha or empty> - atomic replace (or removal)
  if [ -z "$2" ]; then
    rm -f "$ROOT/$1"
    return
  fi
  [ -d "$RELEASES/$2" ] || fail "no release directory for $2"
  ln -sfn "releases/$2" "$ROOT/$1.new"
  mv -Tf "$ROOT/$1.new" "$ROOT/$1"
}

remove_release() { # <sha>
  git -C "$REPO" worktree remove --force "$RELEASES/$1" >/dev/null 2>&1 || true
  rm -rf "${RELEASES:?}/$1"
  git -C "$REPO" worktree prune
}

release_intact() { # <sha> - a clean detached worktree at exactly that commit
  local rel="$RELEASES/$1"
  [ -f "$rel/.git" ] &&
    [ "$(git -C "$rel" rev-parse HEAD 2>/dev/null)" = "$1" ] &&
    [ -z "$(git -C "$rel" status --porcelain --untracked-files=all 2>/dev/null)" ]
}

sandbox() { # <release dir> <label> <command...> - no network, no secrets, no writes
  local dir="$1" label="$2"
  shift 2
  systemd-run --quiet --wait --pipe --collect \
    --unit="tth-gateway-$label-$(date +%s)" \
    -p DynamicUser=yes -p PrivateNetwork=yes -p PrivateTmp=yes -p ProtectSystem=strict \
    -p ProtectHome=yes -p NoNewPrivileges=yes -p CacheDirectory=tth-gateway-build \
    -p WorkingDirectory="$dir" \
    -E DENO_DIR=/var/cache/tth-gateway-build -E DENO_NO_UPDATE_CHECK=1 -E NO_COLOR=1 \
    "$@"
}

prepare_release() { # <sha> <branch>
  local sha="$1" branch="$2" rel="$RELEASES/$1"
  [ "$(git -C "$REPO" remote get-url origin)" = "$REMOTE" ] || fail "$REPO origin is not $REMOTE"
  git -C "$REPO" fetch --quiet --prune origin
  git -C "$REPO" cat-file -e "$sha^{commit}" 2>/dev/null || fail "commit $sha not found on $REMOTE"
  git -C "$REPO" merge-base --is-ancestor "$sha" "refs/remotes/origin/$branch" 2>/dev/null ||
    fail "commit $sha is not on origin/$branch"

  if [ -e "$rel" ] && ! release_intact "$sha"; then
    say "releases/$sha is incomplete or modified - recreating it"
    [ "$sha" != "$(current_sha)" ] || fail "refusing to recreate the ACTIVE release; deploy another commit or roll back first"
    remove_release "$sha"
  fi
  if [ ! -e "$rel" ]; then
    git -C "$REPO" worktree add --quiet --detach "$rel" "$sha" || { remove_release "$sha"; fail "worktree add failed"; }
  fi
  release_intact "$sha" || fail "releases/$sha is not a clean checkout of $sha"
  if ! verify_release "$sha"; then
    [ "$sha" = "$(current_sha)" ] || remove_release "$sha"
    fail "releases/$sha rejected (see above)"
  fi
  say "releases/$sha passed check and tests"
}

verify_release() { # <sha> - files, unit, type-check, tests
  local rel="$RELEASES/$1" f
  for f in gateway/main.ts gateway/deno.json gateway/deploy/tth-gateway.service; do
    [ -f "$rel/$f" ] || { say "commit $1 has no $f"; return 1; }
  done
  systemd-analyze verify "$rel/gateway/deploy/tth-gateway.service" ||
    { say "systemd-analyze verify rejected the unit"; return 1; }
  say "type-check (sandbox) ..."
  sandbox "$rel/gateway" check "$DENO" task check || { say "deno task check failed"; return 1; }
  say "tests (sandbox) ..."
  sandbox "$rel/gateway" test "$DENO" task test || { say "deno task test failed"; return 1; }
}

install_unit_from() { # <sha> - the release's own unit; the old one is backed up
  local src="$RELEASES/$1/gateway/deploy/tth-gateway.service"
  if [ -f "$UNIT_DST" ] && cmp -s "$src" "$UNIT_DST"; then
    return 0
  fi
  if [ -f "$UNIT_DST" ]; then
    install -d -m 0700 "$BACKUPS/units"
    cp -p "$UNIT_DST" "$BACKUPS/units/$SERVICE.$(date -u +%Y%m%dT%H%M%SZ)"
  fi
  install -o root -g root -m 0644 "$src" "$UNIT_DST.new"
  mv -f "$UNIT_DST.new" "$UNIT_DST"
  systemctl daemon-reload
  say "installed $SERVICE from releases/$1"
}

healthy() { # the service is active and /healthz answers "ok" over verified TLS
  local body
  systemctl is-active --quiet "$SERVICE" || return 1
  body="$(curl -fsS --max-time 3 --cacert "$ETC/tls/ca.pem" \
    --resolve "tth-gateway:$PORT:127.0.0.1" "$HEALTH_URL" 2>/dev/null)" || return 1
  [ "$body" = "ok" ]
}

wait_healthy() {
  local i pid
  for ((i = 0; i < HEALTH_TIMEOUT_S; i++)); do
    sleep 1
    healthy && break
  done
  healthy || return 1
  pid="$(systemctl show -p MainPID --value "$SERVICE")"
  sleep "$STABLE_S"
  [ "$(systemctl show -p MainPID --value "$SERVICE")" = "$pid" ] || return 1
  healthy
}

activate() { # <sha> - unit, `current`, restart, health. Returns non-zero on failure.
  install_unit_from "$1" || return 1
  systemctl enable --quiet "$SERVICE" || return 1
  set_link current "$1"
  touch "$RELEASES/$1"
  systemctl restart "$SERVICE" || true
  wait_healthy
}

prune_releases() {
  local cur prev kept=0 d sha
  cur="$(current_sha)"
  prev="$(previous_sha)"
  while IFS= read -r d; do
    sha="$(basename "$d")"
    if [ "$sha" = "$cur" ] || [ "$sha" = "$prev" ] || [ "$kept" -lt "$KEEP" ]; then
      kept=$((kept + 1))
      continue
    fi
    remove_release "$sha"
    say "removed old release $sha"
  done < <(find "$RELEASES" -mindepth 1 -maxdepth 1 -type d -printf '%T@ %p\n' | sort -rn | cut -d' ' -f2-)
}

journal_hint() { say "diagnose with: journalctl -u $SERVICE -n 50 --no-pager"; }

cmd_deploy() {
  local sha="${1:-}" branch=main old
  shift || true
  while [ $# -gt 0 ]; do
    case "$1" in
      --branch) branch="${2:-}"; shift 2 ;;
      *) fail "unknown option $1" ;;
    esac
  done
  [[ "$sha" =~ ^[0-9a-f]{40}$ ]] || fail "deploy needs a full 40-character lowercase commit sha"
  git check-ref-format --branch "$branch" >/dev/null 2>&1 || fail "invalid branch name"
  need_root
  need_tools git curl openssl flock systemd-run systemd-analyze cmp find
  lock
  [ -d "$REPO/.git" ] || fail "no repository at $REPO - run setup first"
  cmd_preflight

  old="$(current_sha)"
  if [ "$old" = "$sha" ] && release_intact "$sha" && healthy; then
    say "$sha is already active and healthy - nothing to do"
    return 0
  fi
  prepare_release "$sha" "$branch"

  say "activating $sha (was: ${old:-none})"
  if activate "$sha"; then
    [ -z "$old" ] || [ "$old" = "$sha" ] || set_link previous "$old"
    log_line "deploy $sha ok (previous ${old:-none})"
    prune_releases
    say "deployed $sha: healthy at $HEALTH_URL"
    return 0
  fi

  log_line "deploy $sha FAILED health check"
  say "deploy of $sha failed the health check"
  journal_hint
  if [ "$old" = "$sha" ]; then
    fail "the active release $sha is still unhealthy; nothing switched (consider: pi_release.sh rollback)"
  fi
  if [ -n "$old" ] && [ -d "$RELEASES/$old" ]; then
    say "rolling back to $old"
    if activate "$old"; then
      log_line "auto-rollback to $old ok"
      remove_release "$sha"
      fail "deploy failed; $old restored and healthy (releases/$sha removed)"
    fi
    log_line "auto-rollback to $old FAILED"
    fail "deploy failed AND the rollback to $old is unhealthy - manual action needed"
  fi
  systemctl stop "$SERVICE" || true
  set_link current ""
  log_line "deploy $sha failed; no earlier release - service stopped"
  fail "deploy failed and there is no earlier release - service stopped"
}

cmd_rollback() {
  local cur prev
  need_root
  need_tools curl flock cmp
  lock
  cur="$(current_sha)"
  prev="$(previous_sha)"
  [ -n "$prev" ] && [ -d "$RELEASES/$prev" ] || fail "no previous release to roll back to"
  release_intact "$prev" || fail "releases/$prev is not a clean checkout - deploy it again instead"
  cmd_preflight
  say "rolling back from ${cur:-none} to $prev"
  if activate "$prev"; then
    [ -z "$cur" ] || set_link previous "$cur"
    log_line "rollback to $prev ok (from ${cur:-none})"
    say "rolled back to $prev: healthy"
    return 0
  fi
  log_line "rollback to $prev FAILED"
  journal_hint
  if [ -n "$cur" ] && [ -d "$RELEASES/$cur" ] && activate "$cur"; then
    fail "rollback to $prev failed; $cur restored and healthy"
  fi
  fail "rollback to $prev failed and $cur is unhealthy - manual action needed"
}

cmd_status() {
  local d
  echo "current:  $(current_sha || true)"
  echo "previous: $(previous_sha || true)"
  echo "releases:"
  if [ -d "$RELEASES" ]; then
    find "$RELEASES" -mindepth 1 -maxdepth 1 -type d -printf '%TY-%Tm-%Td %TH:%TM  %f\n' | sort -r
  fi
  echo "deno:     $({ [ -x "$DENO" ] && "$DENO" --version | head -n1; } || echo missing)"
  echo "service:  $(systemctl is-active "$SERVICE" 2>/dev/null || true) / $(systemctl is-enabled "$SERVICE" 2>/dev/null || true)"
  if [ -r "$ETC/tls/ca.pem" ] && healthy; then echo "health:   ok"; else echo "health:   FAIL (or not checked: run with sudo)"; fi
  if [ -r "$DEPLOY_LOG" ]; then
    echo "last deploys:"
    tail -n 5 "$DEPLOY_LOG" | while IFS= read -r d; do echo "  $d"; done
  fi
}

case "${1:-}" in
  setup) cmd_setup ;;
  preflight) need_root; cmd_preflight ;;
  deploy) shift; cmd_deploy "$@" ;;
  rollback) cmd_rollback ;;
  status) cmd_status ;;
  *)
    sed -n '3,8p' "$0" >&2
    exit 2
    ;;
esac
