# shipped-coverage-test.ps1 -- every shader we SHIP is in a curated list the
# rig judges against the reference.
#
# The release bar (2026-09-06) is "replace the reference for all of our
# shaders", and the denominator of that sentence is the shaders under
# samples/ and sdk/, not the community corpus.  On 2026-09-05 an audit found
# that all 22 shipped fragment shaders were listed and all 21 shipped vertex
# shaders were listed NOWHERE - fourteen sample vpshader.vcg files and the
# SDK's own libdbgfont VP had never been compared against the reference by
# anything.  The first boot that judged them found a wrong-colour defect in a
# shipped sample (hello-ppu-cellgcm-vp-loop).
#
# This test fails, naming the file, whenever a tracked shader under samples/
# or sdk/ is absent from every curated list: reference-pairs.txt,
# path-pairs.txt or path-pair-corpus.txt for a fragment program, vp-pairs.txt
# for a vertex program.  Listed is not judged - the rig's own verdicts are the
# judgement - but an unlisted shader can never be judged, and that is the
# failure this guards.
#
# A shipped shader may be left out only by a reference-corpus-exclude.txt
# record that carries a board id AND a reason (fields path|board id|path|why).
# A bare path in that file exempts nothing here and is itself a failure:
# an exclusion without a reason is the denominator shrinking by silence,
# which is the exact thing this test exists to refuse (review finding,
# codex, 2026-09-06: the first version read only field 0 of every list).
$ErrorActionPreference = "Stop"

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $here "..\..\..")).Path.TrimEnd('\')

function Read-ListPaths([string]$path) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "curated list missing: $path" }
    return @(Get-Content -LiteralPath $path | Where-Object { $_ -and -not $_.StartsWith("#") } |
             ForEach-Object { ($_.Split("|")[0]).Trim() -replace '\\', '/' })
}

# Exclusion RECORDS, not paths: a record exempts a shader only when its board
# id and its reason are both present.  A bare path is returned as malformed
# so the caller can fail on it by name.
function Read-Exclusions([string]$path) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "exclude list missing: $path" }
    $ok = @(); $bad = @()
    foreach ($line in (Get-Content -LiteralPath $path | Where-Object { $_ -and -not $_.StartsWith("#") })) {
        $f = $line.Split("|")
        $rel = ($f[0]).Trim() -replace '\\', '/'
        $id = if ($f.Count -ge 2) { ($f[1]).Trim() } else { "" }
        $why = if ($f.Count -ge 4) { ($f[3]).Trim() } else { "" }
        if ($id -and $why) { $ok += $rel } else { $bad += $rel }
    }
    return [pscustomobject]@{ Excluded = $ok; Malformed = $bad }
}

# Self-check of the exclusion parser against a synthetic file, so a future
# edit cannot quietly widen what counts as a reason.
$probe = Join-Path $env:TEMP ("sd-shipped-coverage-probe-" + [System.Guid]::NewGuid().ToString("N") + ".txt")
try {
    Set-Content -LiteralPath $probe -Value @(
        "# comment",
        "samples/x/shaders/bare.vcg",
        "samples/x/shaders/noreason.vcg|t_00000000|general|",
        "samples/x/shaders/good.vcg|t_00000000|general|poisons every row after it"
    ) -Encoding Ascii
    $r = Read-Exclusions $probe
    # Count FIRST: an array on the left of -ne filters rather than compares, so
    # an EMPTY Excluded would pass a membership assertion silently - a guard
    # must be proven able to fail on the empty case, not only the wrong one
    # (review finding, claude, 2026-09-06).
    if (@($r.Excluded).Count -ne 1 -or @($r.Excluded)[0] -ne "samples/x/shaders/good.vcg") { throw "exclusion parser did not accept exactly the one complete record: [$($r.Excluded -join ',')]" }
    if (@($r.Malformed).Count -ne 2) { throw "exclusion parser did not flag the two reasonless records: $($r.Malformed -join ',')" }
} finally {
    Remove-Item -LiteralPath $probe -Force -ErrorAction SilentlyContinue
}

$prevEap = $ErrorActionPreference
$ErrorActionPreference = "Continue"
$tracked = @(& git -C $repoRoot ls-files -- samples sdk 2>$null)
$gitRc = $LASTEXITCODE
$ErrorActionPreference = $prevEap
if ($gitRc -ne 0 -or $tracked.Count -eq 0) {
    throw "git ls-files yielded nothing under samples/ and sdk/ -- an enumeration problem, not an empty requirement"
}
$shaders = @($tracked | Where-Object { $_ -match '\.(fcg|vcg|cg)$' })
if ($shaders.Count -lt 10) {
    throw "only $($shaders.Count) shipped shaders enumerated; the denominator is wrong, not small"
}

$fragmentLists = @(Read-ListPaths (Join-Path $here "reference-pairs.txt")) +
                 @(Read-ListPaths (Join-Path $here "path-pairs.txt")) +
                 @(Read-ListPaths (Join-Path $here "path-pair-corpus.txt"))
$vertexLists = @(Read-ListPaths (Join-Path $here "vp-pairs.txt"))
$exclusions = Read-Exclusions (Join-Path $here "reference-corpus-exclude.txt")

$missing = @(); $reasonless = @()
$fp = 0; $vp = 0
foreach ($s in $shaders) {
    $isVertex = ($s -match '\.vcg$') -or ($s -match '_v\.cg$')
    if ($isVertex) { $vp++ } else { $fp++ }
    if ($exclusions.Excluded -contains $s) { continue }
    if ($exclusions.Malformed -contains $s) { $reasonless += $s }
    $lists = if ($isVertex) { $vertexLists } else { $fragmentLists }
    if (-not ($lists -contains $s)) { $missing += $s }
}

$problems = @()
if ($reasonless.Count -gt 0) {
    $problems += ("shipped shaders excluded WITHOUT a board id and reason ({0}):`n  {1}" -f $reasonless.Count, ($reasonless -join "`n  "))
}
if ($missing.Count -gt 0) {
    $problems += ("shipped shaders in no curated list ({0} of {1}):`n  {2}" -f $missing.Count, $shaders.Count, ($missing -join "`n  "))
}
if ($problems.Count -gt 0) { throw ($problems -join "`n") }
Write-Host ("shipped-coverage-test: ok - {0} fragment + {1} vertex shipped shaders, every one listed" -f $fp, $vp)
