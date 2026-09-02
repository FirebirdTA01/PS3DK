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
    [string]$ReferencePairs = "",  # default: <rig>/reference-pairs.txt; "-" = none
    # -ReferenceCorpus: also stage EVERY fragment shader under
    # -ReferenceCorpusDir (Windows path; default <repo>/build/shader-corpus,
    # the fetched community corpus, _work/ excluded) as an ours-vs-reference
    # pair under uniform_set auto (increment 3c).  A refusal on either
    # side is a sidecar row, not an abort: the corpus is not curated.
    [switch]$ReferenceCorpus,
    [string]$ReferenceCorpusDir = "",
    # Corpus-relative paths to leave out of the sweep, one per line with
    # the board id that says why (default <rig>/reference-corpus-exclude.txt).
    # A shader goes here only when it poisons the run for every row after
    # it; a plain mismatch or refusal stays in and is reported.
    [string]$ReferenceCorpusExclude = "",
    # -PathPairs: for each shader in -PathPairsList (default <rig>/path-pairs.txt,
    # repo-relative shader|uniform_set) stage our DEFAULT-path container
    # against our GENERAL-path container, preceded by the premise row that
    # makes the default container an oracle: default vs reference.  The
    # guest judges the path-pair row only if that premise judged identical.
    # Needs the reference compiler.  Both containers are compiled here
    # regardless of -GeneralLowering; a refusal on any side aborts (the
    # list is curated).
    [switch]$PathPairs,
    [string]$PathPairsList = "",
    # -PathPairCorpus: the GATE-1 ACCEPTANCE SWEEP for switching the default
    # lowering to the general path.  Every fragment shader under
    # -PathPairCorpusDir (Windows path; default = the reference corpus dir)
    # that the DEFAULT path compiles is staged as a path pair (default vs
    # general, premise row first, uniform_set auto).  A default-path refusal
    # is out of scope for the gate and only counted; a GENERAL-path refusal
    # of a shader the default path compiles IS a gate failure and goes to
    # path-pair-corpus-refused.txt (name|side|rel|rc); a reference refusal
    # leaves the pair unoracled and is counted.  Exclusions come from
    # -ReferenceCorpusExclude rows naming the general path (a general-path
    # poisoner blanks every row after it).  Needs the reference compiler.
    [switch]$PathPairCorpus,
    [string]$PathPairCorpusDir = "",
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
# The lowering path this stage compiles OUR side on.  Curated and exclude
# list rows may name the path they apply to (default | general | both);
# a row for the other path is skipped, not compiled, so a general-only
# regression fixture cannot abort a default-path stage and a general-path
# poisoner cannot hide a shader the default path compiles fine.
$activePath = if ($GeneralLowering) { "general" } else { "default" }
function Row-AppliesToPath([string[]]$fields, [int]$col) {
    $p = if ($fields.Count -gt $col -and $fields[$col].Trim()) { $fields[$col].Trim().ToLower() } else { "both" }
    if ($p -notin @("default", "general", "both")) { throw "list row names an unknown path '$p' (default | general | both): $($fields -join '|')" }
    return ($p -eq "both") -or ($p -eq $activePath)
}

# Auto-Value: the host copy of the guest's auto_value (main.c).  FNV-1a
# over the parameter name, one integer mix per component, then
# 0.125 + m / 2^24 with m < 2^23 - a number every float32 holds EXACTLY,
# printed as its exact decimal so the twin's literal parses back to the
# very same float.  The control-auto row is what proves the two copies
# agree; change one, and that row goes red until the other follows.
# Host copy of the guest's per-name texture permutation (auto_tex_perm):
# FNV-1a of the sampler's name, mod 24, into the lexicographic "RGBA"
# permutation table.  Printed per sampler so a texture-dependent row can
# be read, and so the control twin (sd_tex_baked) can be written for the
# permutation its sampler's name selects.
$script:TexPerms = @('RGBA','RGAB','RBGA','RBAG','RAGB','RABG','GRBA','GRAB','GBRA','GBAR','GARB','GABR',
                     'BRGA','BRAG','BGRA','BGAR','BARG','BAGR','ARGB','ARBG','AGRB','AGBR','ABRG','ABGR')
function Fnv1a([string]$name) {
    [uint64]$mask = 4294967295
    [uint64]$h = 2166136261
    foreach ($b in [System.Text.Encoding]::UTF8.GetBytes($name)) {
        $h = ([uint64]$h -bxor [uint64]$b) -band $mask
        $h = ([uint64]$h * [uint64]16777619) -band $mask
    }
    return [uint64]$h
}
function Auto-TexPerm([string]$name) {
    $p = [int]((Fnv1a $name) % 24)
    return "$p ($($script:TexPerms[$p]))"
}
function Print-AutoTextures([string]$srcPath, [string]$label) {
    $text = Get-Content -Raw -LiteralPath $srcPath
    foreach ($m in [regex]::Matches($text, 'sampler2D\s+([A-Za-z_][A-Za-z0-9_]*)')) {
        Write-Host "stager: texture $label $($m.Groups[1].Value) = perm $(Auto-TexPerm $m.Groups[1].Value)"
    }
}

function Auto-Value([string]$name, [uint32]$k) {
    # Every operand is cast to [uint64] on purpose: PowerShell promotes
    # a mixed unsigned/signed product to Double, which then overflows the
    # -band (measured: the first version of this function produced
    # 4230320.17... for k=0 and errors for the rest).
    [uint64]$mask = 4294967295
    [uint64]$h = Fnv1a $name
    [uint64]$x = ([uint64]$h -bxor (([uint64]$k * [uint64]2654435761) -band $mask)) -band $mask
    $x = ([uint64]$x * [uint64]2246822507) -band $mask
    $x = ([uint64]$x -bxor ([uint64]$x -shr 13)) -band $mask
    [uint64]$m = [uint64]$x -shr 9
    # exact decimal: 0.125 + m / 16777216, both terms dyadic
    [decimal]$v = [decimal]0.125 + ([decimal]$m / [decimal]16777216)
    return $v.ToString([System.Globalization.CultureInfo]::InvariantCulture)
}

# Print the values the guest will synthesise for every float/half vector
# uniform a shader source declares (uniform_set=auto rows).  Read from
# the SOURCE with a regex, not from the container: a curated fixture is
# ours and its declarations are plain.  The point is the fixture, not the
# mechanism: the host and guest hashes agreeing says nothing about
# whether a given NAME yields distinct components, and a fixture whose
# channels collide looks like three tests and is one (review request
# on the lane-extract fixture).  Matrices are listed as not synthesised.
function Print-AutoValues([string]$srcPath, [string]$label) {
    $text = Get-Content -Raw -LiteralPath $srcPath
    $seen = @{}
    $matches = [regex]::Matches($text, 'uniform\s+(float|half)([1-4])?(x[1-4])?\s+([A-Za-z_][A-Za-z0-9_]*)')
    # Say so when nothing matched: a shader with no uniforms and a shader
    # whose declaration the regex missed print the SAME nothing otherwise,
    # and "no COMPONENTS COLLIDE line appeared" would be satisfiable without
    # the check having run (review finding; the same shape as a checker
    # that prints "0 examined, 0 failed" as a pass).
    if ($matches.Count -eq 0) { Write-Host "stager: auto $label no synthesised uniforms declared (regex matched nothing)"; return }
    foreach ($m in $matches) {
        $name = $m.Groups[4].Value
        if ($seen.ContainsKey($name)) { continue }
        $seen[$name] = 1
        if ($m.Groups[3].Success) { Write-Host "stager: auto $label $name = (matrix: not synthesised, row refuses as uniform-unsupported)"; continue }
        $w = if ($m.Groups[2].Success) { [int]$m.Groups[2].Value } else { 1 }
        $vals = @(0..($w - 1) | ForEach-Object { Auto-Value $name $_ })
        $distinct = ($vals | Sort-Object -Unique).Count
        $note = if ($distinct -lt $w) { "  <-- COMPONENTS COLLIDE" } else { "" }
        Write-Host "stager: auto $label $name = $($vals -join ', ')$note"
    }
}

# A shader with a FILE-SCOPE CONST cannot be judged under uniform_set auto
# against the reference today: the reference promotes the const to a
# parameter, the guest's auto walk patches that parameter on the reference
# side, and our folded container has nothing to patch - a mismatch by
# construction (measured: sd_const_promotion@oracle max_delta 9, the
# difference between 0.3125 and its auto value).  Such rows are staged
# under set 0 instead, and the stage says so.  Stays this way: the
# director retired promotion-as-a-flag (22:24), so ours folds without a
# parameter and the reference's promoted one must not be patched.
function Has-FileScopeConst([string]$srcPath) {
    $text = Get-Content -Raw -LiteralPath $srcPath
    # Strip block and line comments, then walk the text tracking brace and
    # parenthesis depth: a `const` counts only at depth 0 of both (outside
    # every function body and every parameter list).  Indentation plays no
    # part (review note, codex: the first version keyed on column 0, which
    # is a convention, not a property).
    $text = [regex]::Replace($text, '/\*[\s\S]*?\*/', ' ')
    $text = [regex]::Replace($text, '//[^\n]*', ' ')
    $depth = 0; $paren = 0
    foreach ($m in [regex]::Matches($text, '[{}()]|\bconst\b')) {
        switch ($m.Value) {
            '{' { $depth++ }
            '}' { if ($depth -gt 0) { $depth-- } }
            '(' { $paren++ }
            ')' { if ($paren -gt 0) { $paren-- } }
            default { if ($depth -eq 0 -and $paren -eq 0) { return $true } }
        }
    }
    return $false
}

# Same binary twice: a curated container must come out byte-identical
# when the same source is compiled again by the same binary.  A compiler
# whose output varies run to run makes every byte fence and every
# staged-vs-judged comparison a comparison of two different programs,
# and nothing downstream can tell (the vita room's wall-clock GUID: months
# of byte-exactness unreachable by construction).  Curated rows only -
# the corpus sweep would double its stage time - and a difference ABORTS
# the stage, since a curated row whose container is not reproducible is
# not a row.
function Assert-Deterministic([string]$src, [string]$firstDst, [string[]]$flags, [switch]$NoExtraFlags, [string]$label) {
    $again = "$firstDst.again"
    # -NoThrow: a second compile that REFUSES where the first succeeded is
    # the most alarming form of nondeterminism, and it must be reported as
    # that, not as the second compile's own empty-container error.
    if (-not (Compile-Shader $src $again $flags -Absolute -NoThrow -NoExtraFlags:$NoExtraFlags)) {
        throw "NONDETERMINISTIC: $label compiled once and refused once under the same binary (rc=$($script:lastCompileRc)); no verdict about it could mean anything"
    }
    $h1 = (Get-FileHash -Algorithm SHA256 -LiteralPath $firstDst).Hash
    $h2 = (Get-FileHash -Algorithm SHA256 -LiteralPath $again).Hash
    Remove-Item -LiteralPath $again -Force -ErrorAction SilentlyContinue
    if ($h1 -ne $h2) { throw "NONDETERMINISTIC: $label compiled twice by the same binary gave different containers ($($h1.Substring(0,12)) vs $($h2.Substring(0,12))); no verdict about it could mean anything" }
}

function Compile-Shader([string]$src, [string]$dst, [string[]]$flags, [switch]$Absolute, [switch]$NoThrow, [switch]$NoExtraFlags) {
    $srcPath = if ($Absolute) { $src } else { Join-Path $here "shaders\$src" }
    # [string[]] on purpose: a one-element array collapses to a String on
    # assignment, and splatting a String splats its CHARACTERS (measured:
    # "- - g e n e r a l ..." reached the compiler).  Passed below as
    # @($pathFlags), the array-subexpression form, never as @pathFlags.
    [string[]]$pathFlags = if ($NoExtraFlags) { @() } else { @($extraFlags) }
    Remove-Item -LiteralPath $dst -Force -ErrorAction SilentlyContinue
    # Our compiler reports a refusal on stderr; under "Stop" a redirected
    # native stderr line is a terminating error (same trap as the
    # reference side), so relax the preference for the call.
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    if ($useWsl) {
        # timeout(1) inside WSL: an uncurated corpus shader must not be
        # able to stall the whole stage on a hung compile.
        $global:LASTEXITCODE = -1
        $null = & wsl -- timeout 30s $WslCompiler @flags @($pathFlags) -p sce_fp_rsx `
            --emit-container (To-WslPath $dst) (To-WslPath $srcPath) 2>&1
    } else {
        $global:LASTEXITCODE = -1
        $null = & $Rsxcgc @flags @($pathFlags) -p sce_fp_rsx --emit-container $dst $srcPath 2>&1
    }
    $rc = $LASTEXITCODE
    $ErrorActionPreference = $prevEap
    # Published for the refusal sidecar: rc 124 is timeout(1) inside WSL,
    # -1 is a launch that never set an exit code - neither is a refusal,
    # and a sidecar that cannot tell them apart once recorded a transient
    # wsl launch failure on draw.fcg as "ours refused".
    $script:lastCompileRc = $rc
    $label = if ($Absolute) { Split-Path -Leaf $src } else { $src }
    # A zero-byte or missing container is a compile that lied about
    # succeeding -- never stage it; the guest would report load-failed.
    $ok = ($rc -eq 0) -and (Test-Path -LiteralPath $dst) -and ((Get-Item $dst).Length -gt 0)
    if (-not $ok) {
        if ($NoThrow) { return $false }
        # Name the flags too: a flagged curated row (t_3bf3ce95) that refuses on
        # a compiler without its flag must say WHICH flag, not just which shader.
        $allFlags = @(@($flags) + @($pathFlags)) | Where-Object { $_ }
        $flagNote = if ($allFlags.Count) { " [flags: $($allFlags -join ' ')]" } else { "" }
        if ($rc -ne 0) { throw "compile failed ($rc): $src$flagNote" }
        throw "compile produced empty container: $src"
    }
    Write-Host "stager: $label -> $(Split-Path -Leaf $dst) ($((Get-Item $dst).Length) bytes)"
    return $true
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

# Texture-binding control (increment 3a): sampled vs arithmetic twin
# of the guest's procedural texture.  Identical iff the auto-binder
# bound the texture where the container's sampler says and nearest
# sampling is texel-aligned; a mismatch withholds every
# sampler-declaring verdict.  --general-lowering on both sides: the
# twin uses floor(), which the default path refuses.
Print-AutoTextures (Join-Path $here "shaders\sd_tex_ctrl.fcg") "control-texture"
Compile-Shader "sd_tex_ctrl.fcg"  (Join-Path $controls "tex_ctrl.fpo")  @("--general-lowering")
Compile-Shader "sd_tex_baked.fcg" (Join-Path $controls "tex_baked.fpo") @("--general-lowering")

# Uniform-synthesis control (increment 3b): side A outputs the uniform
# u_auto under set `auto` (the guest synthesises it from the name);
# side B is a twin GENERATED HERE with the same four values baked as
# exact-decimal literals from Auto-Value.  Identical iff guest and host
# arithmetic agree and the patch path applies the values; a mismatch
# withholds every auto row.  Generated into scratch, never the tree.
$autoScratch = Join-Path $env:TEMP "sd-auto-stage"
New-Item -ItemType Directory -Force $autoScratch | Out-Null
$autoVals = @(0, 1, 2, 3) | ForEach-Object { Auto-Value "u_auto" $_ }
$autoTwin = @(
    "// GENERATED by stage-differential.ps1 - control-auto twin of sd_auto_ctrl.fcg.",
    "// Literals are Auto-Value(""u_auto"", 0..3), exact decimals of float32 values.",
    "void main(out float4 color : COLOR)",
    "{",
    "    color = float4($($autoVals[0])f, $($autoVals[1])f, $($autoVals[2])f, $($autoVals[3])f);",
    "}"
)
$autoTwinPath = Join-Path $autoScratch "sd_auto_baked.fcg"
Set-Content -LiteralPath $autoTwinPath -Value ($autoTwin -join "`n") -Encoding Ascii
Write-Host "stager: control-auto values for u_auto = $($autoVals -join ', ')"
Compile-Shader "sd_auto_ctrl.fcg" (Join-Path $controls "auto_ctrl.fpo")  @("--general-lowering")
Compile-Shader $autoTwinPath      (Join-Path $controls "auto_baked.fpo") @("--general-lowering") -Absolute

$manifest = @(
    "# shader-differential manifest -- generated by stage-differential.ps1",
    "# fields: tier|role|name|a_path|b_path|uniform_set",
    "@target emulator",
    "B|control-identical|ctrl_ident|controls/ctrl_ident_a.fpo|controls/ctrl_ident_b.fpo|0",
    "B|control-mismatch|ctrl_mismatch|controls/ctrl_ident_a.fpo|controls/ctrl_mismatch_b.fpo|0",
    "B|control-uniform|uniform_apply|controls/uniform_ctrl.fpo|controls/uniform_baked.fpo|u1",
    "B|control-texture|texture_bind|controls/tex_ctrl.fpo|controls/tex_baked.fpo|0",
    "B|control-auto|auto_apply|controls/auto_ctrl.fpo|controls/auto_baked.fpo|auto",
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

# Reference-side compile.  The reference compiler reports progress on
# stderr; under $ErrorActionPreference = "Stop" a redirected native
# stderr line is a terminating error in PowerShell 5.1, so relax it for
# the call and judge by exit code + container instead.
# A native command that fails to LAUNCH (non-executable file) does not
# set $LASTEXITCODE, which would otherwise still hold the 0 our own
# compile just returned; pre-set it so the rc check judges this call
# and not the previous one.  The shell's report of a native tool is not
# the tool's report.  It MUST be the $global: form: the engine writes
# the exit code to the GLOBAL variable, and a bare `$LASTEXITCODE = -1`
# creates a shadow in whatever scope this runs in (a script invoked with
# the call operator has its own; measured: a successful reference
# compile then reports -1).  Returns $true iff a non-empty container
# exists afterwards.
function Compile-Reference([string]$src, [string]$dst) {
    Remove-Item -LiteralPath $dst -Force -ErrorAction SilentlyContinue
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $global:LASTEXITCODE = -1
    $null = & $ReferenceCompiler -p sce_fp_rsx -o $dst $src 2>&1
    $refRc = $LASTEXITCODE
    $ErrorActionPreference = $prevEap
    $script:lastCompileRc = $refRc
    return ($refRc -eq 0) -and (Test-Path -LiteralPath $dst) -and ((Get-Item $dst).Length -gt 0)
}

# Ours-vs-reference pairs (increment 2c).  Rows carry role=reference,
# which the guest GATES like a corpus row: a pixel mismatch against the
# reference fails the run.  Byte-identical pairs are counted and not
# staged (byte-identical implies pixel-identical); a refusal on either
# side is a finding, not a skip, so it aborts the stage.
if (-not $ReferenceCompiler -and $env:PS3_REF_CG_COMPILER) { $ReferenceCompiler = $env:PS3_REF_CG_COMPILER }
if ($ReferenceCompiler) {
    # -PathType Leaf enforces the "an executable, never a directory"
    # contract at the layer that can name the fault: a bare Test-Path
    # accepts a directory, the launch then fails, and the next guard
    # blames OUR shader for a bad tool argument.
    if (-not (Test-Path -LiteralPath $ReferenceCompiler -PathType Leaf)) {
        throw "reference compiler not found or not a file (pass -ReferenceCompiler <exe> or set PS3_REF_CG_COMPILER)"
    }
    Write-Host "stager: reference compiler present"
    # -ReferencePairs - (a dash, the manifest's own "none") stages no
    # curated list: the list is tied to the DEFAULT lowering path (a
    # curated shader that refuses is a finding and aborts), while a
    # -GeneralLowering corpus sweep must be able to run without it -
    # discard-blend/fpshader, for one, refuses on the general path until
    # CF-2 lands (t_91bbd575).
    if (-not $ReferencePairs) { $ReferencePairs = Join-Path $here "reference-pairs.txt" }
    $pairLines = @()
    if ($ReferencePairs -ne '-') {
        $pairLines = @(Get-Content $ReferencePairs | Where-Object { $_ -and -not $_.StartsWith("#") })
        if ($pairLines.Count -eq 0) { throw "reference-pairs list is empty: $ReferencePairs" }
    }

    $refDst = Join-Path $root "reference"
    New-Item -ItemType Directory -Force $refDst | Out-Null
    $refScratch = Join-Path $env:TEMP "sd-ref-stage"
    if (Test-Path $refScratch) { Remove-Item -Recurse -Force $refScratch }
    New-Item -ItemType Directory -Force $refScratch | Out-Null

    $refRows = @()
    $refIdentical = 0
    $refSkippedPath = 0
    $seenNames = @{}
    $seenRels = @{}
    foreach ($line in $pairLines) {
        $fields = $line.Split("|")
        $rel = $fields[0].Trim()
        $set = if ($fields.Count -ge 2 -and $fields[1].Trim()) { $fields[1].Trim() } else { "0" }
        if (-not (Row-AppliesToPath $fields 2)) { $refSkippedPath++; continue }
        $src = Join-Path $repoRoot $rel
        if (-not (Test-Path $src)) { throw "reference-pairs: shader not found: $rel" }
        $name = [System.IO.Path]::GetFileNameWithoutExtension($src)
        # The SAME shader listed twice with two uniform sets (a double-duty
        # witness: set 0 for its compiled default, auto for patchability) is
        # named by its set on every listing after the first, so the row and
        # its artifacts say which question they answer.  Two DIFFERENT
        # shaders sharing a basename fall through to the md5 suffix below.
        # (Review finding, codex: the first version only reached the set
        # suffix when the md5 name collided too, which it never does for one
        # path listed twice.)
        if ($seenRels.ContainsKey($rel)) { $name = "$name" + "__set" + $set }
        $seenRels[$rel] = 1
        # Column 4: extra compiler flags for OUR side, space separated,
        # applied on top of the path flag (the reference side has no
        # switches).  A row with flags is named
        # <shader>__<flag-slug>__set<uniform_set>, so the same shader can
        # be staged under two flags, or under one flag with two uniform
        # sets, as distinct rows (the retired flag design listed one shader
        # three times this way; the column stays for the next A/B).
        [string[]]$rowFlags = @()
        if ($fields.Count -ge 4 -and $fields[3].Trim()) {
            $rowFlags = @($fields[3].Trim() -split ' +')
            $slug = (@($rowFlags | ForEach-Object { $_ -replace '^-+', '' }) -join '_') -replace '[^A-Za-z0-9]', '_'
            $name = "$name" + "__" + $slug + "__set" + $set
            if ($name.Length -gt 63) { throw "reference-pairs: row name exceeds the guest's 63-char limit: $name" }
        }
        if ($seenNames.ContainsKey($name)) {
            $md5 = [System.Security.Cryptography.MD5]::Create()
            $hex = ($md5.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($rel)) | ForEach-Object { $_.ToString("x2") }) -join ""
            $name = "$name`_" + $hex.Substring(0, 6)
        }
        $seenNames[$name] = 1

        $ours = Join-Path $refScratch "$name`_ours.fpo"
        $ref  = Join-Path $refScratch "$name`_ref.fpo"
        if ($set -eq "auto") { Print-AutoValues $src $name }
        Print-AutoTextures $src $name
        $null = Compile-Shader $src $ours @($rowFlags) -Absolute
        Assert-Deterministic $src $ours @($rowFlags) -Label $name
        if (-not (Compile-Reference $src $ref)) { throw "reference compile failed or produced no container: $rel" }

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
    Write-Host "stager: reference rows appended ($($refRows.Count) pairs, $refIdentical byte-identical skipped, $refSkippedPath not for the $activePath path)"

    # Reference corpus sweep (increment 3c): every fragment shader under
    # the corpus root, ours vs reference under set auto.  Same naming
    # scheme as stage-corpus.sh (basename, md5-6 of the corpus-relative
    # path on collision); refusals go to reference-corpus-refused.txt
    # (name|side|rel) and byte-identical pairs are counted, not staged.
    if ($ReferenceCorpus) {
        if (-not $ReferenceCorpusDir) { $ReferenceCorpusDir = Join-Path $repoRoot "build\shader-corpus" }
        if (-not (Test-Path -LiteralPath $ReferenceCorpusDir -PathType Container)) {
            throw "reference corpus root not a directory: $ReferenceCorpusDir"
        }
        $corpusRoot = (Resolve-Path -LiteralPath $ReferenceCorpusDir).Path.TrimEnd('\')
        $files = @(Get-ChildItem -LiteralPath $corpusRoot -Recurse -File |
            Where-Object { ($_.Name -like '*.fcg' -or $_.Name -like '*_f.cg') -and ($_.FullName -notlike "*\_work\*") } |
            Sort-Object FullName)
        if ($files.Count -eq 0) { throw "reference corpus is empty: $corpusRoot" }
        if (-not $ReferenceCorpusExclude) { $ReferenceCorpusExclude = Join-Path $here "reference-corpus-exclude.txt" }
        $excluded = @{}
        if (Test-Path -LiteralPath $ReferenceCorpusExclude -PathType Leaf) {
            foreach ($line in (Get-Content $ReferenceCorpusExclude | Where-Object { $_ -and -not $_.StartsWith("#") })) {
                $ef = $line.Split("|")
                if (Row-AppliesToPath $ef 2) { $excluded[$ef[0].Trim()] = $line }
            }
        }
        $cExcluded = 0
        $refusedPath = Join-Path $root "reference-corpus-refused.txt"
        $refusedRows = @("# shader-differential reference-corpus refusals -- generated by stage-differential.ps1", "# fields: name|side|corpus-relative path|rc (0 = compiled but no container; 124 = timeout inside WSL; -1 = launch never returned a code; anything else = the compiler's own refusal code)")
        $cRows = @(); $cIdentical = 0; $cRefusedOurs = 0; $cRefusedRef = 0
        foreach ($f in $files) {
            $rel = $f.FullName.Substring($corpusRoot.Length + 1).Replace('\', '/')
            if ($excluded.ContainsKey($rel)) { $cExcluded++; Write-Host "stager: excluded $rel ($($excluded[$rel]))"; continue }
            $name = $f.Name
            if ($name.EndsWith('.cg')) { $name = $name.Substring(0, $name.Length - 3) }
            if ($name.EndsWith('.fcg')) { $name = $name.Substring(0, $name.Length - 4) }
            if ($seenNames.ContainsKey($name)) {
                $md5 = [System.Security.Cryptography.MD5]::Create()
                $hex = ($md5.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($rel)) | ForEach-Object { $_.ToString("x2") }) -join ""
                $name = "$name`_" + $hex.Substring(0, 6)
            }
            $seenNames[$name] = 1
            $ours = Join-Path $refScratch "$name`_ours.fpo"
            $ref  = Join-Path $refScratch "$name`_ref.fpo"
            $okOurs = Compile-Shader $f.FullName $ours @() -Absolute -NoThrow
            $rcOurs = $script:lastCompileRc
            $okRef  = Compile-Reference $f.FullName $ref
            $rcRef  = $script:lastCompileRc
            if (-not $okOurs) { $cRefusedOurs++; $refusedRows += "$name|ours|$rel|$rcOurs" }
            if (-not $okRef)  { $cRefusedRef++;  $refusedRows += "$name|reference|$rel|$rcRef" }
            if (-not ($okOurs -and $okRef)) { continue }
            $hOurs = (Get-FileHash -Algorithm SHA256 -LiteralPath $ours).Hash
            $hRef  = (Get-FileHash -Algorithm SHA256 -LiteralPath $ref).Hash
            if ($hOurs -eq $hRef) { $cIdentical++; continue }
            Copy-Item $ours (Join-Path $refDst "$name`_ours.fpo") -Force
            Copy-Item $ref  (Join-Path $refDst "$name`_ref.fpo") -Force
            $cRows += "B|reference|$name|reference/$name`_ours.fpo|reference/$name`_ref.fpo|auto"
        }
        Set-Content -LiteralPath $refusedPath -Value ($refusedRows -join "`n") -Encoding Ascii
        if (($cRows.Count + $cIdentical + $cRefusedOurs + $cRefusedRef) -eq 0) { throw "reference corpus sweep compiled nothing" }
        $manifest += $cRows
        Write-Host "stager: reference corpus: $($files.Count) shaders, $($cRows.Count) pairs staged, $cIdentical byte-identical skipped, ours refused $cRefusedOurs, reference refused $cRefusedRef, $cExcluded excluded (sidecar: reference-corpus-refused.txt)"
    }

    # Path pairs: our default-path container vs our general-path container
    # of the same shader, each preceded by its premise row (default vs
    # reference).  Byte-identical default/general pairs are counted, not
    # staged.  The premise is judged in the guest on pixels - the three
    # shaders this was built for are byte-divergent from the reference and
    # pixel-identical to it, so a host-side byte check would call every one
    # of them unoracled.
    if ($PathPairs) {
        if (-not $PathPairsList) { $PathPairsList = Join-Path $here "path-pairs.txt" }
        $ppLines = @(Get-Content $PathPairsList | Where-Object { $_ -and -not $_.StartsWith("#") })
        if ($ppLines.Count -eq 0) { throw "path-pairs list is empty: $PathPairsList" }
        $ppDst = Join-Path $root "pathpair"
        New-Item -ItemType Directory -Force $ppDst | Out-Null
        $ppRows = @(); $ppIdentical = 0
        foreach ($line in $ppLines) {
            $fields = $line.Split("|")
            $rel = $fields[0].Trim()
            $set = if ($fields.Count -ge 2 -and $fields[1].Trim()) { $fields[1].Trim() } else { "0" }
            $src = Join-Path $repoRoot $rel
            if (-not (Test-Path $src)) { throw "path-pairs: shader not found: $rel" }
            $name = [System.IO.Path]::GetFileNameWithoutExtension($src)
            if ($seenNames.ContainsKey($name)) {
                $md5 = [System.Security.Cryptography.MD5]::Create()
                $hex = ($md5.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($rel)) | ForEach-Object { $_.ToString("x2") }) -join ""
                $name = "$name`_" + $hex.Substring(0, 6)
            }
            $seenNames[$name] = 1
            if ($set -eq "auto") { Print-AutoValues $src $name }
            $dDef = Join-Path $refScratch "$name`_default.fpo"
            $dGen = Join-Path $refScratch "$name`_general.fpo"
            $dRef = Join-Path $refScratch "$name`_pathref.fpo"
            $null = Compile-Shader $src $dDef @() -Absolute -NoExtraFlags
            Assert-Deterministic $src $dDef @() -NoExtraFlags -Label "$name (default)"
            $null = Compile-Shader $src $dGen @("--general-lowering") -Absolute -NoExtraFlags
            Assert-Deterministic $src $dGen @("--general-lowering") -NoExtraFlags -Label "$name (general)"
            if (-not (Compile-Reference $src $dRef)) { throw "path-pairs: reference compile failed or produced no container: $rel" }
            $hD = (Get-FileHash -Algorithm SHA256 -LiteralPath $dDef).Hash
            $hG = (Get-FileHash -Algorithm SHA256 -LiteralPath $dGen).Hash
            if ($hD -eq $hG) { $ppIdentical++; Write-Host "stager: $rel default and general containers byte-identical -- not staged"; continue }
            Copy-Item $dDef (Join-Path $ppDst "$name`_default.fpo") -Force
            Copy-Item $dGen (Join-Path $ppDst "$name`_general.fpo") -Force
            Copy-Item $dRef (Join-Path $ppDst "$name`_ref.fpo") -Force
            $ppRows += "B|reference|$name@oracle|pathpair/$name`_default.fpo|pathpair/$name`_ref.fpo|$set"
            $ppRows += "B|path-pair|$name@paths|pathpair/$name`_default.fpo|pathpair/$name`_general.fpo|$set"
        }
        $manifest += $ppRows
        Write-Host "stager: path pairs: $($ppRows.Count / 2) pairs staged (each with its premise row), $ppIdentical byte-identical skipped"
    }

    if ($PathPairCorpus) {
        if (-not $PathPairCorpusDir) {
            $PathPairCorpusDir = if ($ReferenceCorpusDir) { $ReferenceCorpusDir } else { Join-Path $repoRoot "build\shader-corpus" }
        }
        if (-not (Test-Path -LiteralPath $PathPairCorpusDir -PathType Container)) {
            throw "path-pair corpus root not a directory: $PathPairCorpusDir"
        }
        $pcRoot = (Resolve-Path -LiteralPath $PathPairCorpusDir).Path.TrimEnd('\')
        # Relative-path filter, not FullName: the default corpus lives under
        # build/, so a FullName filter on build/ would exclude all of it.
        $pcFiles = @(Get-ChildItem -LiteralPath $pcRoot -Recurse -File |
            Where-Object { $_.Name -like '*.fcg' -or $_.Name -like '*_f.cg' } |
            Where-Object {
                $r = $_.FullName.Substring($pcRoot.Length + 1).Replace('\', '/')
                -not ($r.StartsWith('build/') -or $r.Contains('/_work/') -or $r.StartsWith('_work/'))
            } | Sort-Object FullName)
        if ($pcFiles.Count -eq 0) { throw "path-pair corpus is empty: $pcRoot" }
        $pcExcludeFile = if ($ReferenceCorpusExclude) { $ReferenceCorpusExclude } else { Join-Path $here "reference-corpus-exclude.txt" }
        $pcExcluded = @{}
        if (Test-Path -LiteralPath $pcExcludeFile -PathType Leaf) {
            foreach ($line in (Get-Content $pcExcludeFile | Where-Object { $_ -and -not $_.StartsWith("#") })) {
                $ef = $line.Split("|")
                $ep = if ($ef.Count -gt 2 -and $ef[2].Trim()) { $ef[2].Trim().ToLower() } else { "both" }
                # The general container is always compiled here, so rows
                # naming general or both apply regardless of -GeneralLowering.
                if ($ep -in @("general", "both")) { $pcExcluded[$ef[0].Trim()] = $line }
            }
        }
        $pcDst = Join-Path $root "pathpair"
        New-Item -ItemType Directory -Force $pcDst | Out-Null
        $pcRefusedPath = Join-Path $root "path-pair-corpus-refused.txt"
        $pcRefusedRows = @("# shader-differential path-pair corpus (gate 1) refusals -- generated by stage-differential.ps1",
                           "# fields: name|side|corpus-relative path|rc.  side=general on a shader the default path compiles is a GATE-1 FAILURE;",
                           "#         side=default is out of the gate's scope (the default path refuses it today); side=reference leaves the pair unoracled.")
        $pcRows = @(); $pcStaged = 0; $pcIdentical = 0; $pcDefRefused = 0; $pcGenRefused = 0; $pcRefRefused = 0; $pcExcl = 0; $pcConstSet0 = 0
        foreach ($f in $pcFiles) {
            $rel = $f.FullName.Substring($pcRoot.Length + 1).Replace('\', '/')
            if ($pcExcluded.ContainsKey($rel)) { $pcExcl++; Write-Host "stager: path-pair corpus excluded $rel ($($pcExcluded[$rel]))"; continue }
            $name = $f.Name
            if ($name.EndsWith('.cg')) { $name = $name.Substring(0, $name.Length - 3) }
            if ($name.EndsWith('.fcg')) { $name = $name.Substring(0, $name.Length - 4) }
            if ($seenNames.ContainsKey($name)) {
                $md5 = [System.Security.Cryptography.MD5]::Create()
                $hex = ($md5.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($rel)) | ForEach-Object { $_.ToString("x2") }) -join ""
                $name = "$name" + "_" + $hex.Substring(0, 6)
            }
            $seenNames[$name] = 1
            $dDef = Join-Path $refScratch ("$name" + "_pcdefault.fpo")
            $dGen = Join-Path $refScratch ("$name" + "_pcgeneral.fpo")
            $dRef = Join-Path $refScratch ("$name" + "_pcref.fpo")
            if (-not (Compile-Shader $f.FullName $dDef @() -Absolute -NoThrow -NoExtraFlags)) {
                $pcDefRefused++; $pcRefusedRows += "$name|default|$rel|$($script:lastCompileRc)"; continue
            }
            if (-not (Compile-Shader $f.FullName $dGen @("--general-lowering") -Absolute -NoThrow -NoExtraFlags)) {
                $pcGenRefused++; $pcRefusedRows += "$name|general|$rel|$($script:lastCompileRc)"; continue
            }
            if (-not (Compile-Reference $f.FullName $dRef)) {
                $pcRefRefused++; $pcRefusedRows += "$name|reference|$rel|$($script:lastCompileRc)"; continue
            }
            $hD = (Get-FileHash -Algorithm SHA256 -LiteralPath $dDef).Hash
            $hG = (Get-FileHash -Algorithm SHA256 -LiteralPath $dGen).Hash
            if ($hD -eq $hG) { $pcIdentical++; continue }
            $pcSet = "auto"
            if (Has-FileScopeConst $f.FullName) { $pcSet = "0"; $pcConstSet0++; Write-Host "stager: path-pair corpus $rel has a file-scope const - staged under set 0, not auto (see Has-FileScopeConst)" }
            Copy-Item $dDef (Join-Path $pcDst ("$name" + "_default.fpo")) -Force
            Copy-Item $dGen (Join-Path $pcDst ("$name" + "_general.fpo")) -Force
            Copy-Item $dRef (Join-Path $pcDst ("$name" + "_ref.fpo")) -Force
            $pcRows += "B|reference|$name@oracle|pathpair/$name" + "_default.fpo|pathpair/$name" + "_ref.fpo|$pcSet"
            $pcRows += "B|path-pair|$name@paths|pathpair/$name" + "_default.fpo|pathpair/$name" + "_general.fpo|$pcSet"
            $pcStaged++
        }
        Set-Content -LiteralPath $pcRefusedPath -Value ($pcRefusedRows -join "`n") -Encoding Ascii
        if (($pcStaged + $pcIdentical + $pcDefRefused + $pcGenRefused + $pcRefRefused) -eq 0) { throw "path-pair corpus sweep compiled nothing" }
        $manifest += $pcRows
        Write-Host "stager: path-pair corpus (gate 1): $($pcFiles.Count) shaders, $pcExcl excluded, $pcDefRefused default-refused (out of scope), $pcGenRefused GENERAL-REFUSED (gate failures), $pcRefRefused reference-refused (unoracled), $pcIdentical byte-identical default/general, $pcStaged pairs staged ($pcConstSet0 under set 0 for a file-scope const)"
    }
}

Set-Content -LiteralPath (Join-Path $root "manifest.txt") -Value ($manifest -join "`n") -Encoding Ascii
Write-Host "stager: manifest written ($($manifest.Count) lines) to $root"
Write-Host "stager: done -- run the shader-differential SELF under RPCS3 and scrape SDIFF| rows"
