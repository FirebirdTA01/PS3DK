<#
Hardware boot gate: boots every PSGL sample from build-samples.ps1 on a PS3
through the ps3dk-run network loader (protocol 2), one launch each, and
stops at the first failure.  Nothing is retried.

  boot-samples.ps1 -Selfs <selfs.tsv> -Out <new dir> -Client <ps3dk-run.exe>
                   -Control <control child SELF> -ConsoleIp <ip> -CallbackHost <ip>
                   [-TimeoutSec 90] [-ControlStatus 7]

Order: control, every sample in selfs.tsv, control.  A row passes when
  - the outcome is EXITED with the expected status (the control's own status,
    0 for a sample) and the loader reports ready afterwards;
  - no output was dropped;
  - for a sample, stdout holds its final line from expect.txt exactly once and
    no line from the sample ends in ": fail".
The SELF hash is checked against selfs.tsv before each launch.  Any other
outcome (TIMEOUT, NO_HOOK, DISCONNECTED, ...) stops the gate.  Writes
<Out>/results.tsv and exits 0 only when every row passed.
#>
param(
    [Parameter(Mandatory)][string]$Selfs,
    [Parameter(Mandatory)][string]$Out,
    [Parameter(Mandatory)][string]$Client,
    [Parameter(Mandatory)][string]$Control,
    [Parameter(Mandatory)][string]$ConsoleIp,
    [Parameter(Mandatory)][string]$CallbackHost,
    [int]$TimeoutSec = 90,
    [int]$ControlStatus = 7
)
$ErrorActionPreference = 'Stop'
if (Test-Path $Out) { throw "output exists: $Out" }
New-Item -ItemType Directory $Out | Out-Null

$expect = @{}
foreach ($line in Get-Content "$PSScriptRoot/expect.txt") {
    if ($line -match '^\s*(#|$)') { continue }
    $name, $rx = $line -split '\s+', 2
    $expect[$name] = $rx.Trim()
}
$rows = @(Import-Csv -Delimiter "`t" $Selfs)
if ($rows.Count -eq 0) { throw "no samples in $Selfs" }
foreach ($r in $rows) { if (-not $expect.ContainsKey($r.sample)) { throw "no expect.txt entry for $($r.sample)" } }

$results = @("row`tname`toutcome`tdetail`tready`tdropped`tverdict`treason")
$n = 0
function Invoke-Row([string]$name, [string]$self, [string]$sha, [int]$wantStatus, [string]$rx) {
    $script:n++
    $tag = '{0:d2}-{1}' -f $script:n, $name
    $actual = (Get-FileHash -Algorithm SHA256 $self).Hash.ToLower()
    if ($sha -and $actual -ne $sha) { return @('-', '-', '-', '-', 'FAIL', "SELF hash $actual != $sha") }
    $json = "$Out/$tag.json"
    $p = Start-Process $Client -ArgumentList @($ConsoleIp, "`"$self`"", '--callback-host', $CallbackHost,
        '--timeout', "$TimeoutSec", '--outcome-json', "`"$json`"") -NoNewWindow -PassThru -Wait `
        -RedirectStandardOutput "$Out/$tag.stdout" -RedirectStandardError "$Out/$tag.stderr"
    if (-not (Test-Path $json)) { return @('-', '-', '-', '-', 'FAIL', "no outcome json (client rc $($p.ExitCode))") }
    # Windows PowerShell 5.1 emits a JSON array as one object: unroll it.
    $o = Get-Content -Raw $json | ConvertFrom-Json
    $o = @($o | ForEach-Object { $_ })
    if ($o.Count -ne 1) { return @('-', '-', '-', '-', 'FAIL', "$($o.Count) outcome records") }
    $o = $o[0]
    $cells = @($o.outcome, $o.detail, $o.loader_ready, $o.dropped_bytes)
    if ($o.outcome -ne 'EXITED' -or "$($o.detail)" -ne "$wantStatus") { return $cells + @('FAIL', "want EXITED $wantStatus") }
    if (-not $o.loader_ready) { return $cells + @('FAIL', 'loader not ready') }
    if ([int64]$o.dropped_bytes -ne 0) { return $cells + @('FAIL', 'output dropped') }
    if ($rx -and $rx -ne '-') {
        $lines = @(Get-Content "$Out/$tag.stdout" | ForEach-Object { $_.TrimEnd("`r") })
        $hits = @($lines | Where-Object { $_ -match $rx }).Count
        if ($hits -ne 1) { return $cells + @('FAIL', "final line matched $hits time(s)") }
        if (@($lines | Where-Object { $_ -match "^${name}: .*fail$" }).Count -ne 0) { return $cells + @('FAIL', 'sample reported fail') }
    }
    return $cells + @('PASS', '')
}

$plan = @(@{ name = 'control'; self = $Control; sha = ''; status = $ControlStatus; rx = '-' })
foreach ($r in $rows) { $plan += @{ name = $r.sample; self = $r.self; sha = $r.sha256; status = 0; rx = $expect[$r.sample] } }
$plan += @{ name = 'control'; self = $Control; sha = ''; status = $ControlStatus; rx = '-' }

$ok = $true
foreach ($row in $plan) {
    $res = Invoke-Row $row.name $row.self $row.sha $row.status $row.rx
    $results += ("{0}`t{1}`t{2}" -f $n, $row.name, ($res -join "`t"))
    "{0,2} {1,-30} {2}" -f $n, $row.name, ($res -join ' ')
    if ($res[4] -ne 'PASS') { $ok = $false; break }
}
[System.IO.File]::WriteAllLines("$Out/results.tsv", $results)
if ($ok) { "GATE PASS: $($plan.Count) row(s)"; exit 0 }
"GATE STOPPED at row $n"
exit 1
