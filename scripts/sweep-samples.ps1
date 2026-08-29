param([Parameter(Mandatory=$true)][string]$root)

# Build every bundled sample from an EXTRACTED RELEASE PACKAGE -- i.e. what a
# user actually gets -- rather than from the source tree.  Writes results.csv.
#
#   scripts\sweep-samples.ps1 C:\path\to\ps3-sdk-vX.Y.Z-windows-x86_64
#
# Baseline runs live in docs/sample-sweep-<version>.md.  Building is NOT running:
# a green row here means it links, nothing more.

$env:PS3DK = $root; $env:PS3DEV = $root; $env:PSL1GHT = $root
$env:PATH  = "$root\bin;$env:PATH"
$ninja = 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
$tc    = "$root\cmake\ps3-ppu-toolchain.cmake"
$out   = "C:\ps3sweep"
$csv   = "$out\results.csv"
New-Item -ItemType Directory -Force $out | Out-Null
"family,sample,configure,build,self" | Out-File -Encoding ascii $csv

$samples = Get-ChildItem "$root\samples" -Recurse -Filter CMakeLists.txt |
           Where-Object { $_.FullName -notmatch '\build' } |
           ForEach-Object { $_.Directory }

$i = 0
foreach ($s in $samples) {
    $i++
    $rel  = $s.FullName.Substring("$root\samples\".Length)
    # Split on either separator via a character class.  NOT -split '\' : a lone
    # backslash is an illegal regex and silently blanks the whole column.
    $fam  = ($rel -split '[\\/]')[0]
    $bdir = "$out\b$i"
    $cfg = "fail"; $bld = "skip"; $self = "no"
    $null = cmake -S $s.FullName -B $bdir -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_TOOLCHAIN_FILE=$tc" 2>&1
    if ($LASTEXITCODE -eq 0) {
        $cfg = "ok"
        $null = cmake --build $bdir 2>&1
        if ($LASTEXITCODE -eq 0) { $bld = "ok" } else { $bld = "fail" }
        if (Get-ChildItem $s.FullName -Filter *.fake.self -EA SilentlyContinue) { $self = "yes" }
    }
    "$fam,$($s.Name),$cfg,$bld,$self" | Out-File -Encoding ascii -Append $csv
    Remove-Item $bdir -Recurse -Force -EA SilentlyContinue
    if ($i % 20 -eq 0) { Write-Host "  ...$i/$($samples.Count)" }
}
Write-Host "DONE: $i samples -> $csv"
