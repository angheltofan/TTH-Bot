#!/usr/bin/env bash
# LAN development certificates for the gateway (PHASE6_PLAN §4.2, Step 6.2).
#
#   bash scripts/make_dev_certs.sh <gateway LAN IP or host name> [more names...]
#
# Writes to gateway/.dev-certs/ (ignored by Git):
#   ca.pem        the dev CA certificate  -> provision onto the robot (public)
#   ca.key        the dev CA private key  -> SECRET, stays on this machine
#   gateway.pem   the gateway certificate -> TLS_CERT_FILE
#   gateway.key   the gateway private key -> TLS_KEY_FILE (SECRET)
#
# An existing dev CA is reused, so re-running for a new IP does not require
# re-provisioning the robot. ECDSA P-256 keeps the Core2's handshake short.
#
# Every name is also written as a DNS subjectAltName: this mbedTLS build
# matches the requested host against DNS names only, so an IP address must
# appear as DNS:<ip> as well as IP:<ip>.
set -euo pipefail
cd "$(dirname "$0")/.."

if [ $# -lt 1 ]; then
  echo "usage: bash scripts/make_dev_certs.sh <gateway LAN IP or host name> [more names...]" >&2
  exit 2
fi

out="${TTH_DEV_CERT_DIR:-.dev-certs}"
mkdir -p "$out"
chmod 700 "$out" 2>/dev/null || true
# Git Bash would otherwise rewrite "/CN=..." into a Windows path. With that
# conversion off, openssl (a native Windows program there) needs a Windows
# form of the output directory.
export MSYS_NO_PATHCONV=1
if command -v cygpath >/dev/null 2>&1; then
  out="$(cygpath -m "$out")"
fi

if [ -f "$out/ca.key" ] && [ -f "$out/ca.pem" ]; then
  echo "reusing the existing dev CA in $out"
else
  openssl ecparam -name prime256v1 -genkey -noout -out "$out/ca.key"
  openssl req -x509 -new -key "$out/ca.key" -sha256 -days 3650 \
    -subj "/CN=TTH Bot LAN Dev CA" \
    -addext "basicConstraints=critical,CA:TRUE,pathlen:0" \
    -addext "keyUsage=critical,keyCertSign,cRLSign" \
    -out "$out/ca.pem"
fi

san=""
for name in "$@"; do
  if [[ "$name" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    san="$san,IP:$name,DNS:$name"
  else
    san="$san,DNS:$name"
  fi
done
san="${san#,}"

openssl ecparam -name prime256v1 -genkey -noout -out "$out/gateway.key"
openssl req -new -key "$out/gateway.key" -subj "/CN=$1" -out "$out/gateway.csr"
printf 'basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=serverAuth\nsubjectAltName=%s\n' \
  "$san" > "$out/gateway.ext"
openssl x509 -req -in "$out/gateway.csr" -CA "$out/ca.pem" -CAkey "$out/ca.key" \
  -CAcreateserial -days 825 -sha256 -extfile "$out/gateway.ext" -out "$out/gateway.pem"
rm -f "$out/gateway.csr" "$out/gateway.ext" "$out/ca.srl"
chmod 600 "$out/ca.key" "$out/gateway.key" 2>/dev/null || true

openssl verify -CAfile "$out/ca.pem" "$out/gateway.pem" >/dev/null
echo
echo "written to $out"
echo "  ca.pem       the CA certificate (public)  -> provision onto the robot"
echo "  gateway.pem  the gateway certificate      -> TLS_CERT_FILE"
echo "  gateway.key  the gateway private key      -> TLS_KEY_FILE (secret: never commit, never copy to the robot)"
echo "  ca.key       the CA private key           -> secret: stays in this folder"
echo "  names:       $san"
