<#
Hardware boot gate: boots every PSGL sample from build-samples.ps1 on a PS3
through the ps3dk-run network loader (protocol 2), one launch each, and
stops at the first failure.  Nothing is retried.

  boot-samples.ps1 -Selfs <selfs.tsv> -Out <new dir> -Client <ps3dk-run.exe> -ClientSha <sha256>
                   -Control <control child SELF> -ControlSha <sha256>
                   -ConsoleIp <ip> -CallbackHost <ip> [-TimeoutSec 90] [-ControlStatus 7]

Order: control, every sample in selfs.tsv, control.  Before anything runs,
the client, the control and every sample SELF must match their pinned
sha256, and selfs.tsv must name each sample once with a valid hash.  A row
passes only when all of these hold:
  - the client exited with the expected status (the control's own status,
    0 for a sample);
  - the outcome JSON holds exactly one record: outcome EXITED, detail equal
    to the expected status, protocol 2, loader_ready true, dropped_bytes 0,
    and self_sha256 equal to the pinned hash;
  - the SELF on disk still matches its pin after the run;
  - for a sample, stdout holds its final line from expect.txt exactly once
    and no line from the sample ends in ": fail".
Anything else (TIMEOUT, NO_HOOK, DISCONNECTED, a wrong field, ...) stops
the gate.  Writes <Out>/results.tsv and exits 0 only when every row passed.
#>
param(
    [Parameter(Mandatory)][string]$Selfs,
    [Parameter(Mandatory)][string]$Out,
    [Parameter(Mandatory)][string]$Client,
    [Parameter(Mandatory)][string]$ClientSha,
    [Parameter(Mandatory)][string]$Control,
    [Parameter(Mandatory)][string]$ControlSha,
    [Parameter(Mandatory)][string]$ConsoleIp,
    [Parameter(Mandatory)][string]$CallbackHost,
    [int]$TimeoutSec = 90,
    [int]$ControlStatus = 7
)
$ErrorActionPreference = 'Stop'

function Test-Sha([string]$s) { return $s -cmatch '^[0-9a-f]{64}$' }
function Get-Sha([string]$path) { return (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLower() }

# Judges one finished launch.  Returns @(outcome, detail, ready, dropped,
# verdict, reason); verdict is PASS only when every field matches.
function Test-RowOutcome([string]$name, [string]$self, [string]$sha, [int]$wantStatus,
                         [string]$rx, [int]$clientExit, [string]$json, [string]$stdout) {
    if (-not (Test-Path -LiteralPath $json)) { return @('-', '-', '-', '-', 'FAIL', "no outcome json (client exit $clientExit)") }
    try { $parsed = Get-Content -Raw -LiteralPath $json | ConvertFrom-Json } catch { return @('-', '-', '-', '-', 'FAIL', 'outcome json does not parse') }
    # Windows PowerShell 5.1 emits a JSON array as one object: unroll it.
    $records = @($parsed | ForEach-Object { $_ })
    if ($records.Count -ne 1) { return @('-', '-', '-', '-', 'FAIL', "$($records.Count) outcome records") }
    $o = $records[0]
    $cells = @("$($o.outcome)", "$($o.detail)", "$($o.loader_ready)", "$($o.dropped_bytes)")
    if ($clientExit -ne $wantStatus) { return $cells + @('FAIL', "client exit $clientExit, want $wantStatus") }
    if ("$($o.outcome)" -cne 'EXITED') { return $cells + @('FAIL', 'outcome not EXITED') }
    if ("$($o.detail)" -cne "$wantStatus") { return $cells + @('FAIL', "status $($o.detail), want $wantStatus") }
    if ("$($o.protocol)" -cne '2') { return $cells + @('FAIL', "protocol $($o.protocol), want 2") }
    if ($o.loader_ready -isnot [bool] -or -not $o.loader_ready) { return $cells + @('FAIL', 'loader not ready') }
    if ("$($o.dropped_bytes)" -cne '0') { return $cells + @('FAIL', 'output dropped') }
    if ("$($o.self_sha256)" -cne $sha) { return $cells + @('FAIL', "outcome SELF $($o.self_sha256) != pin") }
    if (-not (Test-Path -LiteralPath $self) -or (Get-Sha $self) -ne $sha) { return $cells + @('FAIL', 'SELF changed during the run') }
    if ($rx -ne '-') {
        $lines = @(Get-Content -LiteralPath $stdout | ForEach-Object { $_.TrimEnd("`r") })
        $hits = @($lines | Where-Object { $_ -match $rx }).Count
        if ($hits -ne 1) { return $cells + @('FAIL', "final line matched $hits time(s)") }
        if (@($lines | Where-Object { $_ -match "^$([regex]::Escape($name)): .*fail$" }).Count -ne 0) { return $cells + @('FAIL', 'sample reported fail') }
    }
    return $cells + @('PASS', '')
}

function Invoke-Row([string]$tag, [string]$name, [string]$self, [string]$sha, [int]$wantStatus, [string]$rx) {
    if (-not (Test-Path -LiteralPath $self) -or (Get-Sha $self) -ne $sha) { return @('-', '-', '-', '-', 'FAIL', 'SELF does not match its pin') }
    $json = "$Out/$tag.json"
    $p = Start-Process $Client -ArgumentList @($ConsoleIp, "`"$self`"", '--callback-host', $CallbackHost,
        '--timeout', "$TimeoutSec", '--outcome-json', "`"$json`"") -NoNewWindow -PassThru -Wait `
        -RedirectStandardOutput "$Out/$tag.stdout" -RedirectStandardError "$Out/$tag.stderr"
    return Test-RowOutcome $name $self $sha $wantStatus $rx ([int]$p.ExitCode) $json "$Out/$tag.stdout"
}

if (Test-Path $Out) { throw "output exists: $Out" }
foreach ($pin in @($ClientSha, $ControlSha)) { if (-not (Test-Sha $pin)) { throw "not a sha256: $pin" } }
if ((Get-Sha $Client) -ne $ClientSha) { throw 'client does not match its pin' }
if ((Get-Sha $Control) -ne $ControlSha) { throw 'control does not match its pin' }

$expect = @{}
foreach ($line in Get-Content "$PSScriptRoot/expect.txt") {
    if ($line -match '^\s*(#|$)') { continue }
    $name, $rx = $line -split '\s+', 2
    $expect[$name] = $rx.Trim()
}
$rows = @(Import-Csv -Delimiter "`t" $Selfs)
if ($rows.Count -eq 0) { throw "no samples in $Selfs" }
$seen = @{}
foreach ($r in $rows) {
    if (-not $r.sample -or $seen.ContainsKey($r.sample)) { throw "missing or duplicate sample name '$($r.sample)'" }
    $seen[$r.sample] = 1
    if (-not $expect.ContainsKey($r.sample)) { throw "no expect.txt entry for $($r.sample)" }
    if (-not (Test-Sha $r.sha256)) { throw "no valid sha256 for $($r.sample)" }
    if ((Get-Sha $r.self) -ne $r.sha256) { throw "$($r.sample) SELF does not match selfs.tsv" }
}
New-Item -ItemType Directory $Out | Out-Null

$plan = @(@{ name = 'control'; self = $Control; sha = $ControlSha; status = $ControlStatus; rx = '-' })
foreach ($r in $rows) { $plan += @{ name = $r.sample; self = $r.self; sha = $r.sha256; status = 0; rx = $expect[$r.sample] } }
$plan += @{ name = 'control'; self = $Control; sha = $ControlSha; status = $ControlStatus; rx = '-' }

$results = @("row`tname`toutcome`tdetail`tready`tdropped`tverdict`treason")
$ok = $true
for ($i = 0; $i -lt $plan.Count; $i++) {
    $row = $plan[$i]
    $tag = '{0:d2}-{1}' -f ($i + 1), $row.name
    $res = Invoke-Row $tag $row.name $row.self $row.sha $row.status $row.rx
    $results += ("{0}`t{1}`t{2}" -f ($i + 1), $row.name, ($res -join "`t"))
    "{0,2} {1,-30} {2}" -f ($i + 1), $row.name, ($res -join ' ')
    if ($res[4] -ne 'PASS') { $ok = $false; break }
}
if ((Get-Sha $Client) -ne $ClientSha) { $ok = $false; 'client changed during the gate' }
[System.IO.File]::WriteAllLines("$Out/results.tsv", $results)
if ($ok) { "GATE PASS: $($plan.Count) row(s)"; exit 0 }
"GATE STOPPED at row $($i + 1)"
exit 1
