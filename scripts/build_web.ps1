<#
.SYNOPSIS
  Controlled release build of the TTH Bot web activity editor.

.DESCRIPTION
  Builds the Flutter web app (lib/main.dart -> AdminAuthGate -> ActivityWebApp)
  into build/web and writes the Vercel deployment configuration next to it.
  build/ is ignored by Git; nothing this script produces is committed.

  -AdminAuthEmail is the internal Supabase Auth email that the visible username
  "admin" maps to. It is compiled into the app (and therefore visible in the
  downloaded JavaScript), but kept out of Git and never printed here. The
  administrator's password is NOT an input of this build and must never be
  passed to it.

  After building, the output is checked for secret-looking content (secret
  keys, JWTs, private keys, service-role references, source maps). The build
  fails if anything is found.

  This script does not deploy. See firmware/core2/docs/PHASE6_PLAN.md §12.5.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts/build_web.ps1 -AdminAuthEmail <internal email>
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)]
  [string] $AdminAuthEmail
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$out = Join-Path $repo 'build\web'

if ($AdminAuthEmail -notmatch '^[^@\s]+@[^@\s]+\.[^@\s]+$') {
  throw 'AdminAuthEmail is not a valid email address.'
}

Push-Location $repo
try {
  Write-Host '[build_web] flutter build web --release (admin auth email: configured)'
  # --no-source-maps: ship no Dart source mapping. Caching is handled by the
  # headers in vercel.json below. The generated flutter_service_worker.js is
  # Flutter's self-unregistering cleanup worker: it removes any service worker
  # a previous deployment left in the browser, so new builds load at once.
  # (The literal "sb_secret_" in main.dart.js is the Supabase SDK's key-prefix
  # check, not a key; the pattern below requires an actual key body.)
  & flutter build web --release --no-source-maps --no-wasm-dry-run `
    "--dart-define=TTH_ADMIN_AUTH_EMAIL=$AdminAuthEmail"
  if ($LASTEXITCODE -ne 0) { throw "flutter build web failed ($LASTEXITCODE)." }

  # Deployment configuration for Vercel, generated into the (ignored) build
  # output that `vercel deploy` uploads.
  $vercel = [ordered]@{
    cleanUrls = $false
    trailingSlash = $false
    headers = @(
      [ordered]@{
        source = '/(.*)'
        headers = @(
          [ordered]@{ key = 'X-Content-Type-Options'; value = 'nosniff' },
          [ordered]@{ key = 'X-Frame-Options'; value = 'DENY' },
          [ordered]@{ key = 'Content-Security-Policy'; value = "frame-ancestors 'none'" },
          [ordered]@{ key = 'Referrer-Policy'; value = 'no-referrer' },
          [ordered]@{ key = 'Permissions-Policy'; value = 'camera=(), microphone=(), geolocation=()' },
          [ordered]@{ key = 'Cache-Control'; value = 'no-cache' }
        )
      },
      [ordered]@{
        source = '/(assets|canvaskit|icons)/(.*)'
        headers = @(
          [ordered]@{ key = 'Cache-Control'; value = 'public, max-age=3600' }
        )
      }
    )
  }
  $json = $vercel | ConvertTo-Json -Depth 6
  [IO.File]::WriteAllText((Join-Path $out 'vercel.json'), $json + "`n", (New-Object Text.UTF8Encoding $false))
  Write-Host '[build_web] wrote build/web/vercel.json'

  # Output checks. Patterns match scripts/check_no_secrets.sh plus web-specific
  # ones. Only file names are printed, never matched content.
  $patterns = @(
    'sb_secret_[0-9A-Za-z_-]{10,}',
    'eyJ[A-Za-z0-9_-]{10,}\.eyJ[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}',
    '-----BEGIN ([A-Z]+ )?PRIVATE KEY-----',
    'AIza[0-9A-Za-z_-]{35}',
    'service_role',
    'GATEWAY_MINT_SECRET',
    'GEMINI_API_KEY=[A-Za-z0-9_-]{8,}'
  )
  $problems = @()
  $files = Get-ChildItem -Path $out -Recurse -File
  foreach ($file in $files) {
    if ($file.Extension -eq '.map') { $problems += "source map present: $($file.Name)"; continue }
    if ($file.Extension -notin '.js', '.html', '.json', '.mjs', '.txt') { continue }
    $text = [IO.File]::ReadAllText($file.FullName)
    foreach ($pattern in $patterns) {
      if ($text -match $pattern) {
        $problems += "secret-like content in $($file.FullName.Substring($out.Length + 1))"
      }
    }
  }
  if ($problems.Count -gt 0) {
    $problems | ForEach-Object { Write-Host "[build_web] FAIL $_" }
    throw 'Build output failed the secret checks.'
  }
  Write-Host "[build_web] checked $($files.Count) files: no secret-like content, no source maps"
  Write-Host '[build_web] done (not deployed)'
}
finally {
  Pop-Location
}
