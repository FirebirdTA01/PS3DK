# manifest-controls-test.ps1 -- the stager refuses a manifest whose proving
# controls are omitted, duplicated or placed after the corpus, and builds a
# correctly ordered one by construction (t_678a4dab).
#
# The guest's MRT/depth gates start closed and open only when the complete
# proving set has run and passed ahead of a row.  These cases pin the host
# half of that contract: what Get-ControlManifestProblems calls a problem,
# what it accepts, and that Add-ProvingControls places the set before the
# first corpus row whatever the manifest looked like.  Every case that must
# refuse is paired with the accepted shape it was derived from, so a check
# that accepted everything or refused everything fails here.
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $here "control-manifest.ps1")

$failures = 0
function Check([string]$name, [bool]$ok, [string]$detail) {
    if ($ok) { Write-Host "PASS $name" } else { Write-Host "FAIL $name -- $detail"; $script:failures++ }
}

$roles = Get-ProvingControlRoles
Check "ten proving roles" ($roles.Count -eq 10) "got $($roles.Count)"

$head = @(
    "# shader-differential manifest",
    "@target emulator",
    "B|control-identical|ctrl_ident|controls/a.fpo|controls/b.fpo|0",
    "B|control-texture|texture_bind|controls/t.fpo|controls/tb.fpo|0"
)
$proving = @($roles | ForEach-Object { "B|$_|$($_ -replace 'control-','')|controls/x.fpo|controls/y.fpo|0" })
$corpus = @(
    "B|corpus|test_01|corpus/a.fpo|corpus/b.fpo|0",
    "B|corpus|test_02|corpus/c.fpo|corpus/d.fpo|0"
)
$tail = @(
    "B|control-calibration|calib|controls/c.fpo|controls/ct.fpo|0",
    "B|control-discard|discard_band|controls/d.fpo|controls/dt.fpo|0"
)

# 1. the accepted shape: proving set before the corpus, other controls anywhere
$good = $head + $proving + $corpus + $tail
$p = Get-ControlManifestProblems $good
Check "complete, ordered manifest accepted" ($p.Count -eq 0) ($p -join "; ")

# 2. one proving control omitted -> exactly one problem naming it
foreach ($drop in @("control-mrt-mismatch", "control-depthonly-identical", "control-sparse-skip")) {
    $m = $head + @($proving | Where-Object { $_ -notmatch "\|$drop\|" }) + $corpus + $tail
    $p = Get-ControlManifestProblems $m
    Check "omitted $drop refused" ($p.Count -eq 1 -and $p[0] -match [regex]::Escape($drop) -and $p[0] -match "missing") ($p -join "; ")
}

# 3. every proving control omitted -> ten problems
$p = Get-ControlManifestProblems ($head + $corpus + $tail)
Check "all proving controls omitted -> ten problems" ($p.Count -eq 10) "got $($p.Count)"

# 4. proving set after the corpus (the pre-fix stager's order) -> ten ordering problems
$p = Get-ControlManifestProblems ($head + $corpus + $proving + $tail)
Check "proving set after corpus refused" ($p.Count -eq 10 -and ($p | Where-Object { $_ -match "after the first corpus row" }).Count -eq 10) ($p -join "; ")

# 5. one proving control moved after the corpus -> exactly one ordering problem
$moved = "control-depth-blind"
$m = $head + @($proving | Where-Object { $_ -notmatch "\|$moved\|" }) + $corpus + @($proving | Where-Object { $_ -match "\|$moved\|" }) + $tail
$p = Get-ControlManifestProblems $m
Check "one control after corpus refused" ($p.Count -eq 1 -and $p[0] -match $moved -and $p[0] -match "after") ($p -join "; ")

# 6. a duplicated proving control -> refused
$p = Get-ControlManifestProblems ($head + $proving + @($proving[0]) + $corpus)
Check "duplicated proving control refused" ($p.Count -eq 1 -and $p[0] -match "more than once") ($p -join "; ")

# 7. no corpus at all: complete set accepted wherever it sits
$p = Get-ControlManifestProblems ($head + $tail + $proving)
Check "control-only manifest accepted" ($p.Count -eq 0) ($p -join "; ")

# 8. Add-ProvingControls places the set before the first corpus row whatever
#    the manifest looked like, and the result passes the check
$built = Add-ProvingControls ($head + $corpus + $tail) $proving
$firstCorpus = [array]::IndexOf($built, $corpus[0])
$lastProving = [array]::IndexOf($built, $proving[-1])
Check "Add-ProvingControls precedes corpus" ($lastProving -ge 0 -and $firstCorpus -gt $lastProving) "corpus at $firstCorpus, last proving at $lastProving"
Check "Add-ProvingControls keeps every line" ($built.Count -eq $head.Count + $corpus.Count + $tail.Count + $proving.Count) "got $($built.Count)"
Check "Add-ProvingControls result accepted" ((Get-ControlManifestProblems $built).Count -eq 0) ""
$builtNoCorpus = Add-ProvingControls ($head + $tail) $proving
Check "Add-ProvingControls appends when no corpus" ($builtNoCorpus[-1] -eq $proving[-1]) ""

# 9. the shipped stager dot-sources this module and refuses on problems
$stager = Get-Content (Join-Path $here "stage-differential.ps1") -Raw
Check "stager dot-sources control-manifest.ps1" ($stager -match 'control-manifest\.ps1') ""
Check "stager calls Add-ProvingControls" ($stager -match 'Add-ProvingControls') ""
Check "stager throws on Get-ControlManifestProblems" ($stager -match 'Get-ControlManifestProblems' -and $stager -match 'throw "manifest: proving controls') ""

if ($failures -gt 0) { Write-Host "manifest-controls-test: $failures failure(s)"; exit 1 }
Write-Host "manifest-controls-test: PASS"
exit 0
