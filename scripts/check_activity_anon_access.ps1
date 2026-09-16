<#
.SYNOPSIS
  Live check of what the PUBLIC anon (publishable) key can do on activities.

.DESCRIPTION
  Uses only the public project URL and publishable key compiled into the app
  (lib/core/supabase/supabase_config.dart). Expected after migration
  20260916120000_activity_admin_writes:

    * SELECT activities                      -> 200
    * INSERT / UPDATE / DELETE               -> refused (401/403, code 42501)
    * the private allow-list over REST       -> not exposed (never 200)
    * activities identical before and after  -> same row count and SHA-256

  It can never modify a real activity, whatever the database state:
    * INSERT sends an invalid body (all required columns null), so even a
      misconfigured database would reject it before a row exists;
    * UPDATE and DELETE target a UUID that matches no activity;
    * TRUNCATE is not reachable through the API and is not attempted.

  Prints only HTTP statuses, error codes, a row count and a hash — never
  titles, prompts, participants or keys.

  -ExpectedHash: compare the activities hash with an earlier run (e.g. the
  baseline taken before the migration or before the administrator's web
  editor test).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts/check_activity_anon_access.ps1
  powershell -ExecutionPolicy Bypass -File scripts/check_activity_anon_access.ps1 -BaselineOnly
  powershell -ExecutionPolicy Bypass -File scripts/check_activity_anon_access.ps1 -ExpectedHash <sha256>
#>
[CmdletBinding()]
param(
  [string] $ExpectedHash,
  # Only read the activities and print count + hash (safe before the migration).
  [switch] $BaselineOnly
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Net.Http

$repo = Split-Path -Parent $PSScriptRoot
$config = [IO.File]::ReadAllText((Join-Path $repo 'lib\core\supabase\supabase_config.dart'))
$url = [regex]::Match($config, "url\s*=\s*'([^']+)'").Groups[1].Value
$key = [regex]::Match($config, "publishableKey\s*=\s*'(sb_publishable_[^']+)'").Groups[1].Value
if (-not $url -or -not $key) { throw 'Could not read the public Supabase URL/publishable key.' }

$client = New-Object System.Net.Http.HttpClient
$client.Timeout = [TimeSpan]::FromSeconds(20)
$failures = 0
$noMatchId = '00000000-0000-0000-0000-000000000000'
$columns = 'id,title,type,prompt,participants,interaction_mode,enabled,sort_order,created_at,updated_at'

function Send([string] $method, [string] $path, [string] $body, [hashtable] $headers) {
  $request = New-Object System.Net.Http.HttpRequestMessage ([System.Net.Http.HttpMethod]::new($method)), "$url/rest/v1/$path"
  $request.Headers.Add('apikey', $key)
  if ($headers) { foreach ($k in $headers.Keys) { $request.Headers.Add($k, $headers[$k]) } }
  if ($null -ne $body) {
    $request.Content = New-Object System.Net.Http.StringContent $body, ([Text.Encoding]::UTF8), 'application/json'
  }
  $response = $client.SendAsync($request).GetAwaiter().GetResult()
  $bytes = $response.Content.ReadAsByteArrayAsync().GetAwaiter().GetResult()
  $code = ''
  try {
    $parsed = [Text.Encoding]::UTF8.GetString($bytes) | ConvertFrom-Json
    if ($parsed.PSObject.Properties.Name -contains 'code') { $code = [string] $parsed.code }
  } catch { }
  [pscustomobject]@{ Status = [int] $response.StatusCode; Code = $code; Bytes = $bytes }
}

function Snapshot {
  $r = Send 'GET' "activities?select=$columns&order=id.asc" $null $null
  if ($r.Status -ne 200) { throw "anon SELECT failed: HTTP $($r.Status) $($r.Code)" }
  $sha = [Security.Cryptography.SHA256]::Create()
  $hash = -join ($sha.ComputeHash($r.Bytes) | ForEach-Object { $_.ToString('x2') })
  # Windows PowerShell emits a JSON array as ONE pipeline object; piping it
  # again unrolls it, so Measure-Object counts the rows.
  $rows = [Text.Encoding]::UTF8.GetString($r.Bytes) | ConvertFrom-Json
  $count = ($rows | Measure-Object).Count
  [pscustomobject]@{ Count = $count; Hash = $hash }
}

function Expect-Refused([string] $label, $r) {
  if (($r.Status -eq 401 -or $r.Status -eq 403) -and $r.Code -eq '42501') {
    Write-Host "PASS $label refused (HTTP $($r.Status), $($r.Code))"
  } else {
    Write-Host "FAIL $label was not refused as expected (HTTP $($r.Status), code '$($r.Code)')"
    $script:failures++
  }
}

try {
  $before = Snapshot
  Write-Host "PASS anon SELECT: $($before.Count) activities, sha256 $($before.Hash)"
  if ($BaselineOnly) { return }

  Expect-Refused 'anon INSERT' (Send 'POST' 'activities' '{"title":null,"type":null,"prompt":null,"interaction_mode":null}' @{ Prefer = 'return=minimal' })
  Expect-Refused 'anon UPDATE' (Send 'PATCH' "activities?id=eq.$noMatchId" '{"sort_order":0}' @{ Prefer = 'return=minimal' })
  Expect-Refused 'anon DELETE' (Send 'DELETE' "activities?id=eq.$noMatchId" $null @{ Prefer = 'return=minimal' })

  $admins = Send 'GET' 'activity_admins?select=user_id' $null @{ 'Accept-Profile' = 'private' }
  if ($admins.Status -eq 200) {
    Write-Host 'FAIL the private allow-list is readable over REST'
    $failures++
  } else {
    Write-Host "PASS private allow-list not exposed (HTTP $($admins.Status), $($admins.Code))"
  }

  $after = Snapshot
  if ($after.Hash -ne $before.Hash -or $after.Count -ne $before.Count) {
    Write-Host 'FAIL activities changed during the check'
    $failures++
  } else {
    Write-Host 'PASS activities unchanged during the check'
  }
  if ($ExpectedHash) {
    if ($after.Hash -eq $ExpectedHash.ToLowerInvariant()) {
      Write-Host 'PASS activities match the expected baseline hash'
    } else {
      Write-Host 'FAIL activities differ from the expected baseline hash'
      $failures++
    }
  }
}
finally {
  $client.Dispose()
}

if ($failures -gt 0) {
  Write-Host "check_activity_anon_access: $failures failure(s)"
  exit 1
}
Write-Host 'check_activity_anon_access: all checks passed'
