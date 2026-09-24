#!/usr/bin/env bash
# Installs the pinned Deno for the Raspberry Pi gateway (gateway/deploy/README.md).
#
#   sudo bash gateway/deploy/install_deno.sh
#
# Deno 2.9.6, aarch64-unknown-linux-gnu, from the official GitHub release, to
#   /usr/local/lib/deno/2.9.6/deno   and   /usr/local/bin/deno -> that binary
#
# No checksum is written in this file. The archive is accepted only when its
# SHA-256 matches BOTH published values:
#   1. the release's own deno-aarch64-unknown-linux-gnu.zip.sha256sum asset;
#   2. the "digest" GitHub computed for the asset (releases API).
# Anything missing, malformed or different -> nothing is installed.
#
# Idempotent: an installed binary that already reports 2.9.6 is kept (only
# the symlink is checked). Another version stays installed side by side, so
# an upgrade or a return to 2.9.6 is a symlink switch.
set -euo pipefail

VERSION="2.9.6"
TARGET="aarch64-unknown-linux-gnu"
ASSET="deno-${TARGET}.zip"
BASE="https://github.com/denoland/deno/releases/download/v${VERSION}"
API="https://api.github.com/repos/denoland/deno/releases/tags/v${VERSION}"
PREFIX="/usr/local/lib/deno/${VERSION}"
LINK="/usr/local/bin/deno"

fail() { echo "install_deno: $*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || fail "run as root (sudo)"
[ "$(uname -s)" = "Linux" ] || fail "Linux only"
[ "$(uname -m)" = "aarch64" ] || fail "expected aarch64, found $(uname -m) - use the 64-bit OS"
for tool in curl unzip sha256sum python3 install mktemp; do
  command -v "$tool" >/dev/null 2>&1 || fail "missing $tool (sudo apt-get install -y curl unzip python3 coreutils)"
done

reports_version() {
  [ -x "$1" ] && "$1" --version 2>/dev/null | head -n1 | grep -qx "deno ${VERSION} (.*)"
}

switch_link() {
  ln -sfn "${PREFIX}/deno" "${LINK}.new"
  mv -Tf "${LINK}.new" "$LINK"
  reports_version "$LINK" || fail "$LINK does not report ${VERSION}"
  echo "install_deno: $LINK -> ${PREFIX}/deno ($("$LINK" --version | head -n1))"
}

if reports_version "${PREFIX}/deno"; then
  echo "install_deno: ${VERSION} already installed in ${PREFIX}"
  switch_link
  exit 0
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"

fetch() { curl --proto '=https' --tlsv1.2 -fsSL --retry 3 -o "$2" "$1" || fail "download failed: $1"; }
fetch "${BASE}/${ASSET}" "$ASSET"
fetch "${BASE}/${ASSET}.sha256sum" "${ASSET}.sha256sum"
fetch "$API" release.json

# Source 1: "<64 hex>  deno-aarch64-unknown-linux-gnu.zip", a single line.
published="$(awk -v a="$ASSET" 'NF == 2 && $2 == a && $1 ~ /^[0-9a-f]{64}$/ { print $1; n++ } END { if (n != 1) exit 1 }' \
  "${ASSET}.sha256sum")" || fail "the published ${ASSET}.sha256sum is not in the expected format"

# Source 2: GitHub's digest for exactly this asset of exactly this release.
digest="$(python3 - "$ASSET" "v${VERSION}" release.json <<'PY'
import json, re, sys
asset, tag, path = sys.argv[1], sys.argv[2], sys.argv[3]
with open(path, encoding="utf-8") as f:
    release = json.load(f)
if release.get("tag_name") != tag:
    sys.exit(1)
found = [a.get("digest", "") for a in release.get("assets", []) if a.get("name") == asset]
if len(found) != 1 or not re.fullmatch(r"sha256:[0-9a-f]{64}", found[0]):
    sys.exit(1)
print(found[0][len("sha256:"):])
PY
)" || fail "the GitHub release digest for ${ASSET} is missing or malformed"

[ "$published" = "$digest" ] || fail "the published checksum and GitHub's digest disagree - refusing"
actual="$(sha256sum "$ASSET" | awk '{ print $1 }')"
[ "$actual" = "$published" ] || fail "checksum mismatch for the downloaded ${ASSET} - refusing"
echo "install_deno: ${ASSET} sha256 ${actual} (matches the release .sha256sum and GitHub's digest)"

unzip -q "$ASSET" deno -d extracted || fail "the archive has no deno binary"
reports_version extracted/deno || fail "the extracted binary does not report ${VERSION}"

install -d -m 0755 "$PREFIX"
install -m 0755 extracted/deno "${PREFIX}/deno.new"
mv -f "${PREFIX}/deno.new" "${PREFIX}/deno"
printf '%s  %s\n' "$actual" "$ASSET" >"${PREFIX}/${ASSET}.sha256"
chmod 0644 "${PREFIX}/${ASSET}.sha256"
switch_link
