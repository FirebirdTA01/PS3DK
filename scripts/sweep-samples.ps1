param([Parameter(Mandatory=$true)][string]$root,
      [string]$out   = "C:\ps3sweep",
      [string]$ninja = 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe',
      [string]$cmake = "cmake",
      [string]$runName = "")

# Build every bundled sample from an EXTRACTED RELEASE PACKAGE -- i.e. what a
# user actually gets -- rather than from the source tree.  Writes results.csv.
#
#   scripts\sweep-samples.ps1 C:\path\to\ps3-sdk-vX.Y.Z-windows-x86_64
#
# Baseline runs live in docs/sample-sweep-<version>.md.  Building is NOT
# running: a green row here means it links, nothing more.
#
# Each sweep writes into its OWN new directory, <out>\run-<time>-<pid>:
#   logs\b<NNN>-<family>_<name>.log   configure + build stdout/stderr and exit
#                                     codes of one sample (t_6ff5b559; the log
#                                     path is the 6th CSV column)
#   results.csv                       this run's table
#   b<NNN>\                           the build dir, removed after each sample
# A fresh directory means no log can hold another sweep's output, even when an
# old log cannot be deleted (held open by an editor or a reader).  The sweep
# refuses to start if the run directory already exists or cannot be created,
# and exits 1 if any log or CSV write failed, so a green run never points at a
# missing or mixed log.  results.csv is also copied to <out>\results.csv and
# the run directory is named in <out>\latest-run.txt.
#
# -cmake and -runName exist for tests/host/sweep-samples-log-test.ps1.

$env:PS3DK = $root; $env:PS3DEV = $root; $env:PSL1GHT = $root
$env:PATH  = "$root\bin;$env:PATH"
$tc    = "$root\cmake\ps3-ppu-toolchain.cmake"
if (-not $runName) { $runName = "run-{0}-{1}" -f (Get-Date -Format "yyyyMMdd-HHmmss"), $PID }
$rundir = Join-Path $out $runName
$logdir = Join-Path $rundir "logs"
$csv    = Join-Path $rundir "results.csv"
if (Test-Path -LiteralPath $rundir) { Write-Host "FAILED: run directory $rundir already exists"; exit 1 }
try {
    New-Item -ItemType Directory -Force $out -ErrorAction Stop | Out-Null
    New-Item -ItemType Directory $rundir -ErrorAction Stop | Out-Null
    New-Item -ItemType Directory $logdir -ErrorAction Stop | Out-Null
    "family,sample,configure,build,self,log" | Out-File -Encoding ascii $csv -ErrorAction Stop
} catch { Write-Host "FAILED: cannot create $rundir ($($_.Exception.Message))"; exit 1 }
$script:writeFailures = 0

try { $samplesRoot = (Get-Item -LiteralPath (Join-Path $root "samples") -ErrorAction Stop).FullName }
catch { Write-Host "FAILED: no samples directory under $root"; exit 1 }
$samples = Get-ChildItem $samplesRoot -Recurse -Filter CMakeLists.txt |
           Where-Object { $_.FullName -notmatch '\build' } |
           ForEach-Object { $_.Directory }

$Rule = "=" * 72
$Dash = "-" * 64
$Stamp = { Get-Date -Format "yyyy-MM-dd HH:mm:ss zzz" }

function Append-Log([string]$log, [string[]]$lines) {
    try { $lines | Out-File -Encoding ascii -Append $log -ErrorAction Stop }
    catch { $script:writeFailures++; Write-Warning "log write failed: $log ($($_.Exception.Message))" }
}

$i = 0
foreach ($s in $samples) {
    $i++
    # Relative to the samples directory as Get-Item spells it: $root may be an 8.3 short path
    # (C:\Users\FIREBI~1\...) while FullName is the long one, and a length taken from $root then
    # cuts the name in the wrong place (its log path pointed into directories that do not exist).
    if (-not $s.FullName.StartsWith($samplesRoot + "\")) { Write-Host "FAILED: $($s.FullName) is not under $samplesRoot"; exit 1 }
    $rel  = $s.FullName.Substring($samplesRoot.Length + 1)
    # Split on either separator via a character class.  NOT -split '\' : a
    # lone backslash is an illegal regex and silently blanks the column.
    $fam  = ($rel -split '[\\/]')[0]
    # One log per sample: b<NNN>-<rel with path separators flattened>.
    # Safe as a Windows filename: the samples tree has no names with
    # spaces or non-alnum path components (verified).
    $relFlat  = $rel -replace '[\\/]', '_'
    $log      = Join-Path $logdir ("b{0:000}-{1}.log" -f $i, $relFlat)
    $bdir     = Join-Path $rundir "b$i"
    $cfg = "fail"; $bld = "skip"; $self = "no"
    $cfg_ec = -1;  $bld_ec = -1

    # Header
    Append-Log $log $Rule
    Append-Log $log ("sample : " + $rel)
    Append-Log $log ("source : " + $s.FullName)
    Append-Log $log ("start  : " + (& $Stamp))
    Append-Log $log $Rule

    # configure (output collected first, so $LASTEXITCODE is cmake's own)
    Append-Log $log ("--- cmake configure  " + $Dash)
    $text = & $cmake -S $s.FullName -B $bdir -G Ninja `
        "-DCMAKE_MAKE_PROGRAM=$ninja" `
        "-DCMAKE_TOOLCHAIN_FILE=$tc" 2>&1 | ForEach-Object { "$_" }
    $cfg_ec = $LASTEXITCODE
    if ($text) { Append-Log $log $text }
    Append-Log $log ("exit   : $cfg_ec")
    if ($cfg_ec -eq 0) { $cfg = "ok" } else { $cfg = "fail" }

    # build (skipped if configure failed)
    if ($cfg -eq "ok") {
        Append-Log $log ("--- cmake build    " + $Dash)
        $text = & $cmake --build $bdir 2>&1 | ForEach-Object { "$_" }
        $bld_ec = $LASTEXITCODE
        if ($text) { Append-Log $log $text }
        Append-Log $log ("exit   : $bld_ec")
        if ($bld_ec -eq 0) { $bld = "ok" } else { $bld = "fail" }
        if (Get-ChildItem $s.FullName -Filter *.fake.self -EA SilentlyContinue) { $self = "yes" }
    }

    # Footer
    Append-Log $log $Dash
    Append-Log $log ("end    : " + (& $Stamp))
    Append-Log $log ("configure : $cfg  (exit $cfg_ec)")
    $bld_exit = if ($bld -eq "skip") { "-" } else { "$bld_ec" }
    Append-Log $log ("build     : $bld  (exit " + $bld_exit + ")")
    Append-Log $log $Rule

    try { "$fam,$($s.Name),$cfg,$bld,$self,$log" | Out-File -Encoding ascii -Append $csv -ErrorAction Stop }
    catch { $script:writeFailures++; Write-Warning "CSV write failed: $csv ($($_.Exception.Message))" }
    Remove-Item $bdir -Recurse -Force -EA SilentlyContinue
    if ($i % 20 -eq 0) { Write-Host "  ...$i/$($samples.Count)  (logs at $logdir)" }
}
try {
    Copy-Item -LiteralPath $csv -Destination (Join-Path $out "results.csv") -Force -ErrorAction Stop
    $rundir | Out-File -Encoding ascii (Join-Path $out "latest-run.txt") -ErrorAction Stop
} catch { $script:writeFailures++; Write-Warning "cannot publish $csv to $out ($($_.Exception.Message))" }
if ($script:writeFailures -gt 0) {
    Write-Host "FAILED: $($script:writeFailures) log/CSV write(s) failed in $rundir; its logs are incomplete"
    exit 1
}
Write-Host "DONE: $i samples -> $csv"
Write-Host "      per-sample logs -> $logdir\bNNN-*.log"
exit 0
