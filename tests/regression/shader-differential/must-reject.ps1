# must-reject.ps1 - frontend acceptance as a rig role (2026-09-02).
#
# The differential rig judges pixels, and a shader the compiler must
# REFUSE has none: this gate lives beside the stager instead of in it,
# and never touches the manifest a boot reads.  For every row of
# must-reject.txt it compiles the case with the reference compiler and
# with ours on the general lowering, classifies each outcome, and holds
# it to the listed value:
#
#   accept           a non-empty container was written
#   frontend-reject  refused, and a diagnostic line NAMES THE SOURCE
#                    FILE (file:line:col) - the compiler-agnostic
#                    discriminator: a backend refusal never names it
#   backend-refuse   refused without naming the file
#
# The reference's verdict is measured every run (its diagnostic text is
# printed to the console and stays out of the tree).  Any row that reads
# other than listed fails the gate; the gate line is MUST_REJECT_OK or
# MUST_REJECT_FAIL, and the exit code follows it, so an acceptance run
# can call this first and stop.  Known divergences from the reference
# are LISTED as what ours does today with the board item in the note,
# so a divergence closing shows as a move here rather than as silence.
#
# Compilers: -WslCompiler <wsl path> (ours; dev builds live in WSL) and
# -ReferenceCompiler <exe> or PS3_REF_CG_COMPILER - the same contract
# as stage-differential.ps1.
param(
    [string]$WslCompiler = "",
    [string]$ReferenceCompiler = "",
    [string]$List = ""
)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $List) { $List = Join-Path $here "must-reject.txt" }
if (-not $ReferenceCompiler -and $env:PS3_REF_CG_COMPILER) { $ReferenceCompiler = $env:PS3_REF_CG_COMPILER }
if (-not $ReferenceCompiler -or -not (Test-Path -LiteralPath $ReferenceCompiler -PathType Leaf)) {
    throw "reference compiler not found or not a file (pass -ReferenceCompiler <exe> or set PS3_REF_CG_COMPILER)"
}
if (-not $WslCompiler) { throw "pass -WslCompiler <wsl path to rsx-cg-compiler>" }
$cases = Join-Path $here "must-reject"
$scratch = Join-Path $env:TEMP "sd-must-reject"
New-Item -ItemType Directory -Force $scratch | Out-Null

function To-WslPath([string]$p) {
    $full = (Resolve-Path -LiteralPath $p).Path
    $drive = $full.Substring(0, 1).ToLowerInvariant()
    return "/mnt/$drive" + $full.Substring(2).Replace('\', '/')
}

# Classify one compile: returns @{ verdict; text; rc }.  The output is
# captured, not discarded, because the verdict is READ FROM IT: a
# refusal whose first diagnostic names the case's file is the frontend's;
# any other refusal is the backend's.  Native stderr under "Stop" would be
# a terminating error, so the preference is relaxed for the call.
function Classify([string]$text, [int]$rc, [string]$dst, [string]$caseName) {
    $has = (Test-Path -LiteralPath $dst) -and ((Get-Item -LiteralPath $dst).Length -gt 0)
    if ($rc -eq 0 -and $has) { return "accept" }
    $names = $false
    foreach ($line in ($text -split "`r?`n")) {
        # ours: "<file>.fcg:157:12: error: ...";  reference: "<file>.fcg(3) : error C1034: ..."
        if ($line -match [regex]::Escape($caseName) -and $line -match "error") { $names = $true; break }
    }
    if ($names) { return "frontend-reject" }
    return "backend-refuse"
}

function Compile-Ours([string]$src, [string]$dst, [string[]]$flags) {
    Remove-Item -LiteralPath $dst -Force -ErrorAction SilentlyContinue
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $global:LASTEXITCODE = -1
    $text = (& wsl -- timeout 30s $WslCompiler @flags -p sce_fp_rsx --emit-container ((To-WslPath $scratch) + "/" + (Split-Path -Leaf $dst)) (To-WslPath $src) 2>&1 | Out-String)
    $rc = $LASTEXITCODE
    $ErrorActionPreference = $prevEap
    # PowerShell prefixes a native stderr line with the launcher's name
    # ("wsl.exe : "); strip it so the printed diagnostic is the tool's.
    $text = ($text -replace "(?m)^wsl\.exe : ", "")
    return @{ rc = $rc; text = $text }
}

function Compile-Ref([string]$src, [string]$dst) {
    Remove-Item -LiteralPath $dst -Force -ErrorAction SilentlyContinue
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $global:LASTEXITCODE = -1
    $text = (& $ReferenceCompiler -p sce_fp_rsx -o $dst $src 2>&1 | Out-String)
    $rc = $LASTEXITCODE
    $ErrorActionPreference = $prevEap
    return @{ rc = $rc; text = $text }
}

function Matches([string]$got, [string]$listed) {
    foreach ($alt in $listed.Split("/")) { if ($got -eq $alt.Trim()) { return $true } }
    return $false
}

function Parse-MustRejectRow([string]$row) {
    $f = $row.Split('|')
    if ($f.Count -gt 4) { throw "must-reject retired five-column schema: migrate to case|reference|ours-general|note: $row" }
    if ($f.Count -lt 3) { throw "must-reject row malformed: $row" }
    # The old note was optional: four fields may still mean legacy/general.
    # Refuse that ambiguous spelling instead of weakening a general verdict.
    $fourthAlternatives = if ($f.Count -eq 4) { @($f[3].Split('/') | ForEach-Object { $_.Trim() }) } else { @() }
    if ($fourthAlternatives.Count -gt 0 -and @($fourthAlternatives | Where-Object { $_ -notmatch '^(accept|frontend-reject|backend-refuse)$' }).Count -eq 0) {
        throw "must-reject retired or ambiguous four-column schema: use a descriptive note, not a verdict: $row"
    }
    return @{ Case=$f[0].Trim(); WantReference=$f[1].Trim(); WantGeneral=$f[2].Trim()
              Note=$(if ($f.Count -eq 4) { $f[3].Trim() } else { '' }) }
}

$rows = @(Get-Content $List | Where-Object { $_ -and -not $_.StartsWith("#") })
if ($rows.Count -eq 0) { throw "must-reject list is empty: $List" }
$fail = 0; $pass = 0
Write-Host "must-reject: $($rows.Count) cases, ours = wsl:$WslCompiler, reference present"
foreach ($row in $rows) {
    # Alternatives in the ours column are written "a/b" so '|' stays the
    # column separator; the note is the fourth field.
    $parsed = Parse-MustRejectRow $row
    $case = $parsed.Case; $wantRef = $parsed.WantReference; $wantGen = $parsed.WantGeneral
    $note = $parsed.Note
    $src = Join-Path $cases "$case.fcg"
    if (-not (Test-Path -LiteralPath $src)) { throw "must-reject case missing: $src" }
    $ref = Compile-Ref $src (Join-Path $scratch "$case.ref.fpo")
    $gotRef = Classify $ref.text $ref.rc (Join-Path $scratch "$case.ref.fpo") $case
    $gen = Compile-Ours $src (Join-Path $scratch "$case.gen.fpo") @()
    $gotGen = Classify $gen.text $gen.rc (Join-Path $scratch "$case.gen.fpo") $case
    # The reference column lists accept | reject: a reject is satisfied by
    # either refusal kind (its diagnostic text is printed, not asserted).
    $okRef = if ($wantRef -eq "reject") { $gotRef -ne "accept" } else { $gotRef -eq $wantRef }
    $okGen = Matches $gotGen $wantGen
    $verdict = if ($okRef -and $okGen) { "PASS" } else { "FAIL" }
    if ($verdict -eq "PASS") { $pass++ } else { $fail++ }
    $refLine = (($ref.text -split "`r?`n") | Where-Object { $_ -match "error" } | Select-Object -First 1)
    Write-Host ("MUSTREJECT|case={0}|reference={1}(want {2})|general={3}(want {4})|{5}{6}" -f `
        $case, $gotRef, $wantRef, $gotGen, $wantGen, $verdict, $(if ($note) { "|note=$note" } else { "" }))
    if ($refLine) { Write-Host "    reference: $($refLine.Trim())" }
    if ($gotGen -ne "accept") { Write-Host "    ours[general]: $((($gen.text -split "`r?`n") | Where-Object { $_ -match 'error|nv40|refus' } | Select-Object -First 1))" }
}
Write-Host "must-reject: $pass pass, $fail fail of $($rows.Count)"
if ($fail -gt 0) { Write-Host "MUST_REJECT_FAIL"; exit 1 }
Write-Host "MUST_REJECT_OK"
exit 0
