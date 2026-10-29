# scripts/sweep-samples.ps1 keeps each sweep's logs to that sweep and fails loudly when it cannot write them
# (codex ac64a1ae: an old log held open could not be cleared, and the sweep appended the new run to it and
# printed DONE).  Drives the sweep against a fake package with two samples and a mock cmake (cmake.cmd):
#   1. a run: exit 0; sample one's log holds its configure + build stdout AND stderr and exit 0, sample two's
#      configure exits 3 (logged, CSV "fail,skip"); the CSV log column points into this run's directory
#   2. a second run with run 1's log held open (FileShare.ReadWrite, as codex's probe): exit 0, a NEW run
#      directory, each log holds exactly one sample header, run 1's log unchanged
#   3. an existing run directory: exit 1 before any cmake call
#   4. a log that cannot be written (the mock puts a directory at sample two's log path): exit 1, FAILED
#   5. <out>\results.csv held with FileShare.None: exit 1, FAILED
# usage: powershell -NoProfile -ExecutionPolicy Bypass -File tests\host\sweep-samples-log-test.ps1
param([string]$Output='')
$ErrorActionPreference = 'Stop'
$repo  = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$sweep = Join-Path $repo 'scripts\sweep-samples.ps1'
$work = if($Output){[IO.Path]::GetFullPath($Output)}else{Join-Path ([IO.Path]::GetTempPath()) ("sweep-log-test-" + [guid]::NewGuid().ToString('N'))}
if(Test-Path -LiteralPath $work){throw 'Fresh fixture root required'}
$passes=0
$fails = 0
function Check([bool]$ok, [string]$what) {
    if ($ok) { Write-Host "sweep-samples-log: ok   $what"; $script:passes++ } else { Write-Host "sweep-samples-log: FAIL $what"; $script:fails++ }
}
try {
    $root = Join-Path $work 'pkg'; $out = Join-Path $work 'out'
    foreach ($n in 'one', 'two') { New-Item -ItemType Directory -Force (Join-Path $root "samples\fam\$n") | Out-Null
        'project(x)' | Out-File -Encoding ascii (Join-Path $root "samples\fam\$n\CMakeLists.txt") }
    $calls = Join-Path $work 'calls.txt'
    New-Item -ItemType Directory (Join-Path $root 'cmake') | Out-Null
    $helper=Join-Path $root 'cmake/ps3-self.cmake'
    'set(PS3_PACKAGING_OUTPUT_LAYOUT "binary-v1")' | Set-Content -LiteralPath $helper
    $mock = Join-Path $work 'cmake.cmd'
    @'
@echo off
echo %* >> "%SWEEP_TEST_CALLS%"
if "%1"=="--build" (
  mkdir "%~2" 2>nul
  if not defined SWEEP_TEST_SOURCEONLY echo self > "%~2\one.fake.self"
  echo asset > "%~2\asset.png"
  echo object > "%~2\retained.o"
  echo BUILD_STDOUT %2
  echo BUILD_STDERR %2 1>&2
  exit /b 0
)
echo CONFIGURE_STDOUT %2
echo CONFIGURE_STDERR %2 1>&2
echo %2 | findstr /c:"two" >nul && exit /b 3
if defined SWEEP_TEST_BREAK_LOG mkdir "%SWEEP_TEST_BREAK_LOG%"
exit /b 0
'@ | Out-File -Encoding ascii $mock
    $env:SWEEP_TEST_CALLS = $calls
    function Sweep([string]$run) {
        $o = & powershell -NoProfile -ExecutionPolicy Bypass -File $sweep -root $root -out $out -cmake $mock -runName $run 2>&1 | ForEach-Object { "$_" }
        return @{ rc = $LASTEXITCODE; text = ($o -join "`n") }
    }
    $L = { param($run, $n) Join-Path $out "$run\logs\b00$n-fam_$(if ($n -eq 1) {'one'} else {'two'}).log" }

    # 1. a clean run
    $r = Sweep 'r1'
    Check ($r.rc -eq 0 -and $r.text -match 'DONE') "run 1 exits 0 with DONE"
    $one = Get-Content -Raw (& $L 'r1' 1); $two = Get-Content -Raw (& $L 'r1' 2)
    Check ($one -match 'CONFIGURE_STDOUT' -and $one -match 'CONFIGURE_STDERR' -and $one -match 'BUILD_STDOUT' -and
           $one -match 'BUILD_STDERR' -and $one -match 'configure : ok  \(exit 0\)' -and $one -match 'build     : ok  \(exit 0\)') "sample one's log holds configure+build stdout and stderr and exit 0"
    Check ($two -match 'CONFIGURE_STDERR' -and $two -match 'exit   : 3' -and $two -match 'configure : fail  \(exit 3\)' -and
           $two -match 'build     : skip  \(exit -\)' -and $two -notmatch 'BUILD_') "sample two's log holds its configure exit 3 and no build"
    $rows = Import-Csv (Join-Path $out 'r1\results.csv')
    Check ($rows.Count -eq 2 -and $rows[0].configure -eq 'ok' -and $rows[0].build -eq 'ok' -and $rows[1].configure -eq 'fail' -and
           $rows[1].build -eq 'skip' -and ($rows | Where-Object { -not $_.log.StartsWith((Join-Path $out 'r1\logs')) }).Count -eq 0) "CSV rows and their log column point into run 1"
    Check ((Get-Content -Raw (Join-Path $out 'latest-run.txt')).Trim() -eq (Join-Path $out 'r1')) "latest-run.txt names run 1"
    Check ($rows[0].self -eq 'yes' -and (Test-Path (Join-Path $out 'r1/b1/one.fake.self'))) 'binary SELF is reported and retained'
    Check ((Test-Path (Join-Path $out 'r1/b1/asset.png')) -and (Test-Path (Join-Path $out 'r1/b1/retained.o'))) 'assets and intermediate evidence are retained'

    # 2. a second run while run 1's log is held open for append (deletion denied)
    $before = Get-Content -Raw (& $L 'r1' 1)
    $hold = [IO.File]::Open((& $L 'r1' 1), 'Open', 'ReadWrite', 'ReadWrite')
    try { $r = Sweep 'r2' } finally { $hold.Dispose() }
    Check ($r.rc -eq 0) "run 2 exits 0 with run 1's log held"
    foreach ($n in 1, 2) { $t = Get-Content -Raw (& $L 'r2' $n)
        Check (([regex]::Matches($t, 'sample : ')).Count -eq 1) "run 2 log $n holds exactly one sample header" }
    Check ((Get-Content -Raw (& $L 'r1' 1)) -eq $before) "run 1's log is unchanged"

    # 3. an existing run directory is refused before any cmake call
    Remove-Item $calls -Force -ErrorAction SilentlyContinue
    $r = Sweep 'r1'
    Check ($r.rc -eq 1 -and $r.text -match 'already exists' -and -not (Test-Path $calls)) "an existing run directory: exit 1, no cmake call"

    # 4. a log write that fails: sample two's log path is a directory
    $env:SWEEP_TEST_BREAK_LOG = & $L 'r4' 2
    try { $r = Sweep 'r4' } finally { Remove-Item Env:SWEEP_TEST_BREAK_LOG }
    Check ($r.rc -eq 1 -and $r.text -match 'FAILED') "an unwritable log: exit 1, FAILED"

    # 5. the published results.csv held exclusively
    $hold = [IO.File]::Open((Join-Path $out 'results.csv'), 'Open', 'ReadWrite', 'None')
    try { $r = Sweep 'r5' } finally { $hold.Dispose() }
    Check ($r.rc -eq 1 -and $r.text -match 'FAILED') "an unwritable published results.csv: exit 1, FAILED"

    # A stale source output must not make a fresh binary build look successful.
    'stale' | Set-Content (Join-Path $root 'samples/fam/one/one.fake.self')
    $env:SWEEP_TEST_SOURCEONLY='1'
    try {$r=Sweep 'source-only'} finally {Remove-Item Env:SWEEP_TEST_SOURCEONLY}
    $rows=Import-Csv (Join-Path $out 'source-only/results.csv')
    Check ($rows[0].self -eq 'no') 'source-only stale SELF is never accepted'

    '# older helper without binary layout support' | Set-Content -LiteralPath $helper
    Remove-Item -LiteralPath $calls -Force
    $r=Sweep 'old-sdk'
    Check ($r.rc -ne 0 -and $r.text -match 'older.*SDK|SDK.*older|output layout' -and -not(Test-Path $calls)) 'older SDK refuses before any configure/build'
} finally {
    if(-not $Output) {
        $tempRoot=[IO.Path]::GetFullPath([IO.Path]::GetTempPath())
        if(-not ([IO.Path]::GetFullPath($work)).StartsWith($tempRoot,[StringComparison]::OrdinalIgnoreCase)){throw 'Unsafe cleanup path'}
        Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
    }
}
Write-Host "sweep-samples-log: $passes pass, $fails fail"
if ($fails -eq 0) { Write-Host "PASS: sweep-samples-log-test"; exit 0 } else { Write-Host "FAIL: sweep-samples-log-test ($fails)"; exit 1 }
