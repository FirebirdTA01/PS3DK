# stage-differential.ps1 -- host-side stager for the shader-differential rig.
#
# Compiles the standing-control and probe fragment shaders with OUR
# compiler, stages the resulting containers plus the manifest into
# RPCS3's dev_hdd0, and creates the artifacts directory the guest dumps
# mismatches into.  Corpus pairs (including reference-compiled
# containers, which live only in private host-side storage and are
# staged as runtime input) are appended by later increments; this
# script owns the invariant that rows 1..2 of the manifest are the two
# standing controls, in order.
#
# The control containers travel the same runtime-load path as every
# corpus pair on purpose: controls that were embedded at build time
# would validate a different code path than the one they exist to
# validate.
#
# Compiler resolution prefers the working-tree build over the
# installed release: this rig exists to judge the compiler under
# development, and %PS3DK% is a clean release extract by standing rule.

param(
    [string]$Rsxcgc = "",
    [string]$Rpcs3Path = "C:\Users\FirebirdTA01\Desktop\Emulators\RPCS3\rpcs3.exe",
    [switch]$GeneralLowering,
    # -Corpus: also stage the ours-vs-ours fast/nofast corpus sweep
    # (increment 2).  The corpus compile loop runs in WSL via
    # stage-corpus.sh, because dev builds of the compiler live there —
    # pass the compiler's WSL path in -WslCompiler.  Refused shaders
    # land in the ours-refused.txt sidecar; only byte-differing pairs
    # are staged (byte-identical implies pixel-identical).
    [switch]$Corpus,
    [string]$WslCompiler = "",
    [string]$CorpusDir = "",     # WSL path; default: repo corpus + tracked fixtures
    # -ReferenceCompiler: also stage the ours-vs-reference pairs listed in
    # -ReferencePairs (increment 2c).  A host-side executable, or the
    # PS3_REF_CG_COMPILER environment variable (the same discovery
    # contract as the oracle harness: an executable, never a directory,
    # and the console reports presence, never the path).  Reference
    # containers are runtime input under dev_hdd0 only; they never enter
    # the tree.  Byte-identical pairs are not staged.
    [string]$ReferenceCompiler = "",
    [string]$ReferencePairs = "",  # default: <rig>/reference-pairs.txt
    [string]$Hdd0 = ""           # override dev_hdd0 root (testing)
)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = (Resolve-Path (Join-Path $here "..\..\..")).Path

function To-WslPath([string]$p) {
    $q = $p -replace '\\', '/'
    if ($q -match '^([A-Za-z]):(.*)$') {
        return "/mnt/$($Matches[1].ToLower())$($Matches[2])"
    }
    throw "cannot translate to WSL path: $p"
}

# Compiler resolution, explicit before discovered.  Dev builds of the
# compiler live in WSL on this host, so an EXPLICIT -WslCompiler
# routes every compile (controls included) through `wsl --` and beats
# the discovery fallbacks — the %PS3DK% release extract in particular
# must never silently outrank a dev compiler the caller named (it did,
# in this script's first -Corpus run: the release refused the MAD
# probe and the run judged the wrong compiler).  Judging the compiler
# under development is the rig's whole point.
$useWsl = $false
if ($Rsxcgc) {
    # explicit native exe wins outright
} elseif ($WslCompiler) {
    $useWsl = $true
} else {
    $candidates = @(
        (Join-Path $repoRoot "tools\rsx-cg-compiler\build\rsx-cg-compiler.exe"),
        (Join-Path $env:PS3DK "bin\rsx-cg-compiler.exe")
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { $Rsxcgc = $c; break }
    }
    if (-not $Rsxcgc) {
        throw "rsx-cg-compiler not found (checked working-tree build and %PS3DK%\bin); pass -Rsxcgc or -WslCompiler"
    }
}
if ($useWsl) { Write-Host "stager: compiler = wsl:$WslCompiler" }
else         { Write-Host "stager: compiler = $Rsxcgc" }

$hdd0 = $Hdd0
if (-not $hdd0) { $hdd0 = Join-Path (Split-Path -Parent $Rpcs3Path) "dev_hdd0" }
if (-not (Test-Path $hdd0)) { throw "dev_hdd0 not found at $hdd0; pass -Rpcs3Path or -Hdd0" }
$root = Join-Path $hdd0 "shader-differential"
$controls = Join-Path $root "controls"
$artifacts = Join-Path $root "artifacts"
New-Item -ItemType Directory -Force $controls | Out-Null
New-Item -ItemType Directory -Force $artifacts | Out-Null

$extraFlags = @()
if ($GeneralLowering) { $extraFlags += "--general-lowering" }

function Compile-Shader([string]$src, [string]$dst, [string[]]$flags, [switch]$Absolute) {
    $srcPath = if ($Absolute) { $src } else { Join-Path $here "shaders\$src" }
    if ($useWsl) {
        & wsl -- $WslCompiler @flags @($extraFlags) -p sce_fp_rsx `
            --emit-container (To-WslPath $dst) (To-WslPath $srcPath)
    } else {
        & $Rsxcgc @flags @($extraFlags) -p sce_fp_rsx --emit-container $dst $srcPath
    }
    if ($LASTEXITCODE -ne 0) { throw "compile failed ($LASTEXITCODE): $src" }
    # A zero-byte container is a compile that lied about succeeding --
    # refuse to stage it rather than let the guest report load-failed.
    if ((Get-Item $dst).Length -eq 0) { throw "compile produced empty container: $src" }
    $label = if ($Absolute) { Split-Path -Leaf $src } else { $src }
    Write-Host "stager: $label -> $(Split-Path -Leaf $dst) ($((Get-Item $dst).Length) bytes)"
}

# Standing controls.  control-identical is ONE container byte-copied
# twice -- the identity of the pair is established by the copy, not by
# compiling twice (compile-twice would quietly also test compiler
# determinism, which is a different question with its own harness).
Compile-Shader "sd_ctrl_a.fcg" (Join-Path $controls "ctrl_ident_a.fpo") @()
Copy-Item (Join-Path $controls "ctrl_ident_a.fpo") (Join-Path $controls "ctrl_ident_b.fpo") -Force
Compile-Shader "sd_ctrl_b.fcg" (Join-Path $controls "ctrl_mismatch_b.fpo") @()

# MAD-fusion contraction probe: our compiler against itself with
# fusion toggled.  The row's verdict answers "is the documented
# exception class to the bit-identical gate populated, as visible to
# an A8R8G8B8 RT" -- see sd_mad_probe.fcg for the channel design that
# separates contraction from coarser fastmath effects.
# Pinned to --general-lowering on BOTH sides: the fastmath question
# was measured on the general path, and the default path has no frac
# recognizer so the probe cannot ride it at all (first actual
# execution of this line found that; increment 1's manifest was
# hand-written and never exercised it).
Compile-Shader "sd_mad_probe.fcg" (Join-Path $controls "probe_mad_fast.fpo") @("--general-lowering")
Compile-Shader "sd_mad_probe.fcg" (Join-Path $controls "probe_mad_nofast.fpo") @("--general-lowering", "--nofastmath")

# Uniform-application control (increment 2b): output-is-uniform vs the
# same value baked as a literal, under set u1.  Identical iff the
# guest's cellGcmSetFragmentProgramParameter path actually patches the
# embedded constant; silent non-application judges mismatch and the
# guest then refuses to judge uniform-dependent rows.
Compile-Shader "sd_uniform_ctrl.fcg"  (Join-Path $controls "uniform_ctrl.fpo") @("--general-lowering")
Compile-Shader "sd_uniform_baked.fcg" (Join-Path $controls "uniform_baked.fpo") @("--general-lowering")
$uniforms = @(
    "# shader-differential uniform sets -- generated by stage-differential.ps1",
    "# fields: set|name|x,y,z,w",
    "u1|u_color|0.75,0.25,0.5,1.0"
)
Set-Content -LiteralPath (Join-Path $root "uniforms.txt") -Value ($uniforms -join "`n") -Encoding Ascii

$manifest = @(
    "# shader-differential manifest -- generated by stage-differential.ps1",
    "# fields: tier|role|name|a_path|b_path|uniform_set",
    "@target emulator",
    "B|control-identical|ctrl_ident|controls/ctrl_ident_a.fpo|controls/ctrl_ident_b.fpo|0",
    "B|control-mismatch|ctrl_mismatch|controls/ctrl_ident_a.fpo|controls/ctrl_mismatch_b.fpo|0",
    "B|control-uniform|uniform_apply|controls/uniform_ctrl.fpo|controls/uniform_baked.fpo|u1",
    "B|probe|mad_fusion|controls/probe_mad_fast.fpo|controls/probe_mad_nofast.fpo|0"
)

if ($Corpus) {
    if (-not $WslCompiler) {
        throw "-Corpus needs -WslCompiler <wsl path to rsx-cg-compiler> (dev builds live in WSL)"
    }
    $helper = Join-Path $here "stage-corpus.sh"
    $stage  = Join-Path $env:TEMP "sd-corpus-stage"
    if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
    New-Item -ItemType Directory -Force $stage | Out-Null

    if (-not $CorpusDir) { $CorpusDir = To-WslPath (Join-Path $repoRoot "build\shader-corpus") }
    $helperWsl = To-WslPath $helper
    $stageWsl  = To-WslPath $stage

    Write-Host "stager: corpus sweep via WSL ($WslCompiler over $CorpusDir)"
    & wsl -- bash $helperWsl $WslCompiler $CorpusDir $stageWsl
    if ($LASTEXITCODE -ne 0) { throw "stage-corpus.sh failed ($LASTEXITCODE)" }

    $corpusDst = Join-Path $root "corpus"
    New-Item -ItemType Directory -Force $corpusDst | Out-Null
    $stagedFiles = Get-ChildItem (Join-Path $stage "corpus") -File -ErrorAction SilentlyContinue
    foreach ($f in $stagedFiles) { Copy-Item $f.FullName $corpusDst -Force }
    foreach ($side in "ours-refused.txt", "flag-verdict-changes.txt") {
        Copy-Item (Join-Path $stage $side) (Join-Path $root $side) -Force
    }
    $corpusRows = Get-Content (Join-Path $stage "manifest-corpus.txt") |
        Where-Object { $_ -and -not $_.StartsWith("#") }
    $manifest += $corpusRows
    Write-Host "stager: corpus rows appended ($($corpusRows.Count) pairs, $($stagedFiles.Count) containers)"
}

# Ours-vs-reference pairs (increment 2c).  Rows carry role=reference,
# which the guest GATES like a corpus row: a pixel mismatch against the
# reference fails the run.  Byte-identical pairs are counted and not
# staged (byte-identical implies pixel-identical); a refusal on either
# side is a finding, not a skip, so it aborts the stage.
if (-not $ReferenceCompiler -and $env:PS3_REF_CG_COMPILER) { $ReferenceCompiler = $env:PS3_REF_CG_COMPILER }
if ($ReferenceCompiler) {
    if (-not (Test-Path $ReferenceCompiler)) {
        throw "reference compiler not found (pass -ReferenceCompiler <exe> or set PS3_REF_CG_COMPILER)"
    }
    Write-Host "stager: reference compiler present"
    if (-not $ReferencePairs) { $ReferencePairs = Join-Path $here "reference-pairs.txt" }
    $pairLines = @(Get-Content $ReferencePairs | Where-Object { $_ -and -not $_.StartsWith("#") })
    if ($pairLines.Count -eq 0) { throw "reference-pairs list is empty: $ReferencePairs" }

    $refDst = Join-Path $root "reference"
    New-Item -ItemType Directory -Force $refDst | Out-Null
    $refScratch = Join-Path $env:TEMP "sd-ref-stage"
    if (Test-Path $refScratch) { Remove-Item -Recurse -Force $refScratch }
    New-Item -ItemType Directory -Force $refScratch | Out-Null

    $refRows = @()
    $refIdentical = 0
    $seenNames = @{}
    foreach ($line in $pairLines) {
        $fields = $line.Split("|")
        $rel = $fields[0].Trim()
        $set = if ($fields.Count -ge 2 -and $fields[1].Trim()) { $fields[1].Trim() } else { "0" }
        $src = Join-Path $repoRoot $rel
        if (-not (Test-Path $src)) { throw "reference-pairs: shader not found: $rel" }
        $name = [System.IO.Path]::GetFileNameWithoutExtension($src)
        if ($seenNames.ContainsKey($name)) {
            $md5 = [System.Security.Cryptography.MD5]::Create()
            $hex = ($md5.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($rel)) | ForEach-Object { $_.ToString("x2") }) -join ""
            $name = "$name`_" + $hex.Substring(0, 6)
        }
        $seenNames[$name] = 1

        $ours = Join-Path $refScratch "$name`_ours.fpo"
        $ref  = Join-Path $refScratch "$name`_ref.fpo"
        Compile-Shader $src $ours @() -Absolute
        # The reference compiler reports progress on stderr; under
        # $ErrorActionPreference = "Stop" a redirected native stderr line
        # is a terminating error in PowerShell 5.1, so relax it for the
        # call and judge by exit code + container instead.
        $prevEap = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        $null = & $ReferenceCompiler -p sce_fp_rsx -o $ref $src 2>&1
        $refRc = $LASTEXITCODE
        $ErrorActionPreference = $prevEap
        if ($refRc -ne 0) { throw "reference compile failed ($refRc): $rel" }
        if (-not (Test-Path $ref) -or (Get-Item $ref).Length -eq 0) { throw "reference compile produced no container: $rel" }

        $hOurs = (Get-FileHash -Algorithm SHA256 -LiteralPath $ours).Hash
        $hRef  = (Get-FileHash -Algorithm SHA256 -LiteralPath $ref).Hash
        if ($hOurs -eq $hRef) {
            $refIdentical++
            Write-Host "stager: $rel byte-identical to reference -- not staged"
            continue
        }
        Copy-Item $ours (Join-Path $refDst "$name`_ours.fpo") -Force
        Copy-Item $ref  (Join-Path $refDst "$name`_ref.fpo") -Force
        $refRows += "B|reference|$name|reference/$name`_ours.fpo|reference/$name`_ref.fpo|$set"
    }
    $manifest += $refRows
    Write-Host "stager: reference rows appended ($($refRows.Count) pairs, $refIdentical byte-identical skipped)"
}

Set-Content -LiteralPath (Join-Path $root "manifest.txt") -Value ($manifest -join "`n") -Encoding Ascii
Write-Host "stager: manifest written ($($manifest.Count) lines) to $root"
Write-Host "stager: done -- run the shader-differential SELF under RPCS3 and scrape SDIFF| rows"
