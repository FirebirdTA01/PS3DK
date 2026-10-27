<#
Checks boot-samples.ps1's row judgement without a console: the functions are
taken from the script's syntax tree and Start-Process is replaced by a stub
that writes a chosen outcome JSON, stdout and client exit status.  One good
row must PASS; every case below changes one thing and must FAIL with the
expected reason.  The first negative case is the reviewer's mock (wrong
outcome SELF hash, protocol 99, client exit 126).

  check-harness.ps1 -Out <new dir>
Prints CHECK_HARNESS_OK cases=N, or CHECK_HARNESS_FAIL lines, and exits 0
only when every case behaved as expected.
#>
param([Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
if (Test-Path $Out) { throw "output exists: $Out" }
New-Item -ItemType Directory $Out | Out-Null

$source = "$PSScriptRoot/boot-samples.ps1"
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($source, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw 'boot-samples.ps1 does not parse' }
foreach ($fn in @('Test-Sha', 'Get-Sha', 'Test-RowOutcome', 'Invoke-Row')) {
    $def = $ast.Find({ param($a) $a -is [Management.Automation.Language.FunctionDefinitionAst] -and $a.Name -eq $fn }, $true)
    if (-not $def) { throw "no function $fn in boot-samples.ps1" }
    Invoke-Expression $def.Extent.Text
}

$self = "$Out/sample.self"
[IO.File]::WriteAllBytes($self, [byte[]](1, 2, 3, 4))
$sha = (Get-FileHash -Algorithm SHA256 $self).Hash.ToLower()
$Client = 'never-executed'; $ConsoleIp = 'unused'; $CallbackHost = 'unused'; $TimeoutSec = 1
$name = 'hello-psgl-cg-binary'
$rx = '^hello-psgl-cg-binary: done$'

# Stub state for the current case.
$script:StubJson = ''; $script:StubStdout = ''; $script:StubExit = 0; $script:StubTouchSelf = $false
function Start-Process {
    param($FilePath, $ArgumentList, [switch]$NoNewWindow, [switch]$PassThru, [switch]$Wait, $RedirectStandardOutput, $RedirectStandardError)
    $json = $ArgumentList[-1].Trim('"')
    if ($null -ne $script:StubJson) { [IO.File]::WriteAllText($json, $script:StubJson) }
    [IO.File]::WriteAllText($RedirectStandardOutput, $script:StubStdout)
    [IO.File]::WriteAllText($RedirectStandardError, '')
    if ($script:StubTouchSelf) { [IO.File]::WriteAllBytes($self, [byte[]](9, 9, 9, 9)) }
    [pscustomobject]@{ ExitCode = $script:StubExit }
}

function Record([hashtable]$over) {
    $r = [ordered]@{ index = 0; run_id = 1; outcome = 'EXITED'; detail = '0'; self_sha256 = $sha
                     dropped_bytes = 0; loader_ready = $true; protocol = 2 }
    foreach ($k in $over.Keys) { if ($null -eq $over[$k]) { $r.Remove($k) } else { $r[$k] = $over[$k] } }
    return $r
}
$goodOut = "PSGL_SAMPLE_BOOT hook=1`nhello-psgl-cg-binary: ready`nhello-psgl-cg-binary: done`n"

$cases = @(
    @{ id = 'good';                 want = 'PASS'; json = (ConvertTo-Json -Compress @((Record @{}))) },
    @{ id = 'reviewer-mock';        want = 'client exit 126'; exit = 126; json = (ConvertTo-Json -Compress @((Record @{ self_sha256 = 'WRONG'; protocol = 99 }))) },
    @{ id = 'client-exit';          want = 'client exit 126'; exit = 126; json = (ConvertTo-Json -Compress @((Record @{}))) },
    @{ id = 'outcome-timeout';      want = 'outcome not EXITED'; json = (ConvertTo-Json -Compress @((Record @{ outcome = 'TIMEOUT' }))) },
    @{ id = 'status';               want = 'status 1'; json = (ConvertTo-Json -Compress @((Record @{ detail = '1' }))) },
    @{ id = 'protocol-99';          want = 'protocol 99'; json = (ConvertTo-Json -Compress @((Record @{ protocol = 99 }))) },
    @{ id = 'protocol-missing';     want = 'protocol , want 2'; json = (ConvertTo-Json -Compress @((Record @{ protocol = $null }))) },
    @{ id = 'not-ready';            want = 'loader not ready'; json = (ConvertTo-Json -Compress @((Record @{ loader_ready = $false }))) },
    @{ id = 'ready-as-string';      want = 'loader not ready'; json = (ConvertTo-Json -Compress @((Record @{ loader_ready = 'true' }))) },
    @{ id = 'dropped';              want = 'output dropped'; json = (ConvertTo-Json -Compress @((Record @{ dropped_bytes = 5 }))) },
    @{ id = 'outcome-sha';          want = 'outcome SELF'; json = (ConvertTo-Json -Compress @((Record @{ self_sha256 = ('0' * 64) }))) },
    @{ id = 'two-records';          want = '2 outcome records'; json = (ConvertTo-Json -Compress @((Record @{}), (Record @{}))) },
    @{ id = 'zero-records';         want = '0 outcome records'; json = '[]' },
    @{ id = 'no-json';              want = 'no outcome json'; json = $null },
    @{ id = 'bad-json';             want = 'does not parse'; json = '[{"outcome":' },
    @{ id = 'final-line-missing';   want = 'matched 0 time'; json = (ConvertTo-Json -Compress @((Record @{}))); stdout = "hello-psgl-cg-binary: ready`n" },
    @{ id = 'final-line-twice';     want = 'matched 2 time'; json = (ConvertTo-Json -Compress @((Record @{}))); stdout = $goodOut + "hello-psgl-cg-binary: done`n" },
    @{ id = 'fail-line';            want = 'sample reported fail'; json = (ConvertTo-Json -Compress @((Record @{}))); stdout = $goodOut + "hello-psgl-cg-binary: fail`n" },
    @{ id = 'self-changed-in-run';  want = 'SELF changed'; json = (ConvertTo-Json -Compress @((Record @{}))); touch = $true },
    @{ id = 'pin-mismatch-before';  want = 'does not match its pin'; json = (ConvertTo-Json -Compress @((Record @{}))); pin = ('f' * 64) }
)

$bad = 0
$rows = @("case`twant`tverdict`treason")
foreach ($c in $cases) {
    [IO.File]::WriteAllBytes($self, [byte[]](1, 2, 3, 4))
    $script:StubJson = $c.json
    $script:StubStdout = if ($c.ContainsKey('stdout')) { $c.stdout } else { $goodOut }
    $script:StubExit = if ($c.ContainsKey('exit')) { $c.exit } else { 0 }
    $script:StubTouchSelf = [bool]$c.touch
    $pin = if ($c.ContainsKey('pin')) { $c.pin } else { $sha }
    $res = Invoke-Row "case-$($c.id)" $name $self $pin 0 $rx
    $verdict = $res[4]; $reason = $res[5]
    $good = if ($c.want -eq 'PASS') { $verdict -eq 'PASS' } else { $verdict -eq 'FAIL' -and $reason -like "*$($c.want)*" }
    $rows += "$($c.id)`t$($c.want)`t$verdict`t$reason"
    if (-not $good) { $bad++; "CHECK_HARNESS_FAIL $($c.id): got $verdict '$reason', want '$($c.want)'" }
}
[IO.File]::WriteAllLines("$Out/cases.tsv", $rows)
if ($bad -eq 0) { "CHECK_HARNESS_OK cases=$($cases.Count)"; exit 0 }
exit 1
