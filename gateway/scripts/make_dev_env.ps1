# Writes gateway/.env for LAN DEVELOPMENT (Step 6.2 device tests).
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File gateway\scripts\make_dev_env.ps1
#
# Asks for the robot's device id and the token_sha256 digest that
# tools/provision.py printed, so no value ever has to be typed into a command
# line or a tracked file. Refuses to write anything unless Git ignores
# gateway/.env. The digest is a SHA-256 of the robot's token, not the token.
#
# The file sets GEMINI_MODE=fake: no Gemini, no token mint, no mint secret.

$ErrorActionPreference = "Stop"

$gateway = Split-Path -Parent $PSScriptRoot
$envFile = Join-Path $gateway ".env"

& git -C $gateway check-ignore -q .env
if ($LASTEXITCODE -ne 0) {
  throw "gateway/.env is NOT ignored by Git - refusing to write it"
}
Write-Host "gateway/.env is ignored by Git"

foreach ($name in @("gateway.pem", "gateway.key")) {
  if (-not (Test-Path (Join-Path $gateway ".dev-certs\$name"))) {
    throw "gateway/.dev-certs/$name is missing - run gateway/scripts/make_dev_certs.sh first"
  }
}

if (Test-Path $envFile) {
  $answer = Read-Host "gateway/.env already exists. Type yes to replace it"
  if ($answer -ne "yes") {
    Write-Host "gateway/.env unchanged"
    exit 1
  }
}

$deviceId = (Read-Host "Device id provisioned on the robot").Trim()
if ($deviceId -notmatch '^[A-Za-z0-9_-]{1,32}$') {
  throw "device id: 1-32 characters of A-Z a-z 0-9 _ -"
}
$digest = (Read-Host "token_sha256 printed by provision.py (64 hexadecimal characters)").Trim().ToLowerInvariant()
if ($digest -notmatch '^[0-9a-f]{64}$') {
  throw "token_sha256 must be exactly 64 hexadecimal characters"
}

# Single quotes keep the JSON intact for Deno's --env-file parser.
$devices = "'{""$deviceId"":{""token_sha256"":""$digest"",""activity_id"":null,""enabled"":true}}'"
$lines = @(
  "# LAN development only. Written by gateway/scripts/make_dev_env.ps1; ignored by Git.",
  "GEMINI_MODE=fake",
  "PORT=8443",
  "TLS_CERT_FILE=.dev-certs/gateway.pem",
  "TLS_KEY_FILE=.dev-certs/gateway.key",
  "TTH_DEVICES=$devices"
)
[System.IO.File]::WriteAllLines($envFile, $lines, (New-Object System.Text.UTF8Encoding($false)))
Write-Host "wrote gateway/.env (device $deviceId, GEMINI_MODE=fake, port 8443)"
