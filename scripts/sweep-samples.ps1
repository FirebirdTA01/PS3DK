param([Parameter(Mandatory=$true)][string]$root)

# Build every bundled sample from an EXTRACTED RELEASE PACKAGE -- i.e. what a
# user actually gets -- rather than from the source tree.  Writes results.csv.
#
#   scripts\sweep-samples.ps1 C:\path\to\ps3-sdk-vX.Y.Z-windows-x86_64
#
# Baseline runs live in docs/sample-sweep-<version>.md.  Building is NOT
# running: a green row here means it links, nothing more.
#
# Per-sample configure + build stdout/stderr are captured to
#   <out>\logs\b<NNN>-<family>_<name>.log
# (t_6ff5b559 -- the log path is the 6th CSV column).  This lets an
# intermittent build failure be diagnosed from what cmake actually printed
# rather than from "exit code was non-zero" with no stderr.
#
# The build dir itself is still removed at the end of each iteration to keep
# $out small; only the log persists.

$env:PS3DK = $root; $env:PS3DEV = $root; $env:PSL1GHT = $root
$env:PATH  = "$root\bin;$env:PATH"
$ninja = 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
$tc    = "$root\cmake\ps3-ppu-toolchain.cmake"
$out   = "C:\ps3sweep"
$csv   = "$out\results.csv"
$logdir = "$out\logs"
New-Item -ItemType Directory -Force $out    | Out-Null
New-Item -ItemType Directory -Force $logdir | Out-Null
"family,sample,configure,build,self,log" | Out-File -Encoding ascii $csv

$samples = Get-ChildItem "$root\samples" -Recurse -Filter CMakeLists.txt |
           Where-Object { $_.FullName -notmatch '\build' } |
           ForEach-Object { $_.Directory }

$Rule = "=" * 72
$Dash = "-" * 64
$Stamp = { Get-Date -Format "yyyy-MM-dd HH:mm:ss zzz" }

function Append-Log([string]$log, [string]$line) {
    $line | Out-File -Encoding ascii -Append $log
}

$i = 0
foreach ($s in $samples) {
    $i++
    $rel  = $s.FullName.Substring("$root\samples\".Length)
    # Split on either separator via a character class.  NOT -split '\' : a
    # lone backslash is an illegal regex and silently blanks the column.
    $fam  = ($rel -split '[\\/]')[0]
    # One log per sample: b<NNN>-<rel with path separators flattened>.
    # Safe as a Windows filename — the samples tree has no names with
    # spaces or non-alnum path components (verified).
    $relFlat  = $rel -replace '[\\/]', '_'
    $log      = Join-Path $logdir ("b{0:000}-{1}.log" -f $i, $relFlat)
    $bdir     = "$out\b$i"
    $cfg = "fail"; $bld = "skip"; $self = "no"
    $cfg_ec = -1;  $bld_ec = -1

    # Header
    Append-Log $log $Rule
    Append-Log $log ("sample : " + $rel)
    Append-Log $log ("source : " + $s.FullName)
    Append-Log $log ("start  : " + & $Stamp)
    Append-Log $log $Rule

    # configure
    Append-Log $log ("--- cmake configure  " + $Dash)
    cmake -S $s.FullName -B $bdir -G Ninja `
        "-DCMAKE_MAKE_PROGRAM=$ninja" `
        "-DCMAKE_TOOLCHAIN_FILE=$tc" 2>&1 | Out-File -Encoding ascii -Append $log
    $cfg_ec = $LASTEXITCODE
    Append-Log $log ("exit   : $cfg_ec")
    if ($cfg_ec -eq 0) { $cfg = "ok" } else { $cfg = "fail" }

    # build (skipped if configure failed)
    if ($cfg -eq "ok") {
        Append-Log $log ("--- cmake build    " + $Dash)
        cmake --build $bdir 2>&1 | Out-File -Encoding ascii -Append $log
        $bld_ec = $LASTEXITCODE
        Append-Log $log ("exit   : $bld_ec")
        if ($bld_ec -eq 0) { $bld = "ok" } else { $bld = "fail" }
        if (Get-ChildItem $s.FullName -Filter *.fake.self -EA SilentlyContinue) { $self = "yes" }
    }

    # Footer
    Append-Log $log $Dash
    Append-Log $log ("end    : " + & $Stamp)
    Append-Log $log ("configure : $cfg  (exit $cfg_ec)")
    Append-Log $log ("build     : $bld  (exit " + (if ($bld -eq "skip") { "-" } else { "$bld_ec" }) + ")")
    Append-Log $log $Rule

    "$fam,$($s.Name),$cfg,$bld,$self,$log" | Out-File -Encoding ascii -Append $csv
    Remove-Item $bdir -Recurse -Force -EA SilentlyContinue
    if ($i % 20 -eq 0) { Write-Host "  ...$i/$($samples.Count)  (logs at $logdir)" }
}
Write-Host "DONE: $i samples -> $csv"
Write-Host "      per-sample logs -> $logdir\bNNN-*.log"
