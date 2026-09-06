# stage-roots-test.ps1 -- the stager's three roots resolve to the tree they
# belong to (t_b1269234).
#
# The coupling this pins: the stager used to resolve every repo-relative
# path against the tree its own code lived in, while -WslCompiler pointed
# at a worktree's binary, so a branch fixture could not be judged from its
# branch, and a same-named file in the shared tree could be judged in its
# place and called a verdict.  Measured on the pre-change stager: it even
# ACCEPTED -RepoRoot <x> without a word (a plain param() block swallows
# unknown parameters) and judged its own tree.
#
# Every case below builds its trees under TEMP; nothing here reads the
# repository this test lives in except the helper it dot-sources.
$ErrorActionPreference = "Stop"

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $here "path-pair-corpus-inputs.ps1")

$work = Join-Path $env:TEMP ("sd-stage-roots-test-" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force $work | Out-Null
# Canonical (long-form) so a relative -RepoRoot, which the provider resolves
# to the long form, compares equal to paths built from $work below.
Push-Location -LiteralPath $work; try { $work = (Get-Location).Path } finally { Pop-Location }

# A tree shaped like a repository as far as the stager cares: the rig dir
# with a curated list, and a stager "script dir" inside it.
function New-Tree([string]$name, [bool]$withCorpus) {
    $root = Join-Path $work $name
    $rig = Join-Path $root "tests\regression\shader-differential"
    New-Item -ItemType Directory -Force $rig | Out-Null
    Set-Content -LiteralPath (Join-Path $rig "reference-pairs.txt") -Value "# $name" -Encoding Ascii
    if ($withCorpus) {
        New-Item -ItemType Directory -Force (Join-Path $root "build\shader-corpus") | Out-Null
    }
    return $root
}

try {
    $script = New-Tree "script-tree" $false
    $scriptRig = Join-Path $script "tests\regression\shader-differential"

    # 1. No -RepoRoot: the judged tree IS the script's tree, and with no
    #    corpus anywhere the corpus root stays under it, flagged.
    $r = Resolve-StageRoots -ScriptDir $scriptRig
    if ($r.RepoRoot -ne $script) { throw "default RepoRoot is not the script's tree: $($r.RepoRoot)" }
    if ($r.Rig -ne $scriptRig) { throw "default Rig is not the script's rig dir: $($r.Rig)" }
    if ($r.CorpusRoot -ne (Join-Path $script "build\shader-corpus")) { throw "unfetched corpus root should stay under the judged tree: $($r.CorpusRoot)" }
    if ($r.CorpusNote -notlike "NOT FETCHED*") { throw "unfetched corpus was not flagged: $($r.CorpusNote)" }
    $banner = @(Format-StageRootsBanner $r)
    if ($banner.Count -ne 2 -or $banner[0] -ne "stager: tree = $script" -or $banner[1] -notlike "stager: corpus = *NOT FETCHED*") {
        throw "same-tree banner wrong: $($banner -join ' | ')"
    }

    # 2. -RepoRoot at another tree: every tree-owned root moves there, the
    #    banner names both trees, and that tree's own corpus wins.
    $judged = New-Tree "judged-tree" $true
    $r = Resolve-StageRoots -ScriptDir $scriptRig -RepoRoot $judged
    if ($r.ScriptTree -ne $script) { throw "ScriptTree moved with -RepoRoot: $($r.ScriptTree)" }
    if ($r.RepoRoot -ne $judged) { throw "-RepoRoot not honoured: $($r.RepoRoot)" }
    if ($r.Rig -ne (Join-Path $judged "tests\regression\shader-differential")) { throw "Rig did not follow -RepoRoot: $($r.Rig)" }
    if ($r.CorpusRoot -ne (Join-Path $judged "build\shader-corpus") -or $r.CorpusNote -ne "the judged tree's own fetch") {
        throw "judged tree's own corpus not preferred: $($r.CorpusRoot) ($($r.CorpusNote))"
    }
    $banner = @(Format-StageRootsBanner $r)
    if ($banner[0] -ne "stager: tree = $judged (judged tree; the stager code runs from $script)") {
        throw "cross-tree banner wrong: $($banner[0])"
    }

    # 3. A relative, forward-slashed -RepoRoot resolves (To-WslPath needs a
    #    drive-letter absolute path downstream).
    Push-Location $work
    try {
        $r = Resolve-StageRoots -ScriptDir $scriptRig -RepoRoot "./judged-tree"
    } finally { Pop-Location }
    if ($r.RepoRoot -ne $judged) { throw "relative -RepoRoot did not resolve: $($r.RepoRoot)" }

    # 4. Explicit -ReferenceCorpusDir outranks every fetch.
    $explicit = Join-Path $work "explicit-corpus"
    New-Item -ItemType Directory -Force $explicit | Out-Null
    $r = Resolve-StageRoots -ScriptDir $scriptRig -RepoRoot $judged -ReferenceCorpusDir $explicit
    if ($r.CorpusRoot -ne $explicit -or $r.CorpusNote -ne "-ReferenceCorpusDir") {
        throw "explicit corpus dir not honoured: $($r.CorpusRoot) ($($r.CorpusNote))"
    }

    # 5. A linked worktree without a fetch falls back to the PRIMARY
    #    checkout's corpus - the case that lets a worktree stage without a
    #    refetch of its own.  Built as a real git repository with a real
    #    linked worktree, because that is the only way git's common dir
    #    exists to be found.
    $primary = New-Tree "primary" $true
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $null = & git -C $primary init -q 2>&1
    $null = & git -C $primary -c user.name=t -c user.email=t@t add -A 2>&1
    $null = & git -C $primary -c user.name=t -c user.email=t@t commit -q -m tree 2>&1
    $linked = Join-Path $work "linked"
    $null = & git -C $primary worktree add -q --detach $linked 2>&1
    $ErrorActionPreference = $prevEap
    if (-not (Test-Path -LiteralPath (Join-Path $linked "tests\regression\shader-differential\reference-pairs.txt"))) {
        throw "could not build a linked worktree for the primary-fallback case"
    }
    $r = Resolve-StageRoots -ScriptDir $scriptRig -RepoRoot $linked
    if ($r.CorpusRoot -ne (Join-Path $primary "build\shader-corpus") -or $r.CorpusNote -notlike "the primary checkout's fetch*") {
        throw "linked worktree did not fall back to the primary's corpus: $($r.CorpusRoot) ($($r.CorpusNote))"
    }

    # 6. A root that is not a rig tree, or not a directory, is refused by
    #    name rather than staging nothing from the wrong place.
    foreach ($bad in @((Join-Path $work "explicit-corpus"), (Join-Path $work "no-such-dir"))) {
        $refused = $false
        try { $null = Resolve-StageRoots -ScriptDir $scriptRig -RepoRoot $bad } catch { $refused = ($_.Exception.Message -like "-RepoRoot *") }
        if (-not $refused) { throw "bad -RepoRoot was not refused by name: $bad" }
    }

    # 7. Curated rows: corpus rows go through the corpus root, everything
    #    else through the judged tree, whichever slash the list used.
    $p = Resolve-StageTreePath -RepoRoot $judged -CorpusRoot $explicit -Rel "build/shader-corpus/rsxgl/files/x.fcg"
    if ($p -ne (Join-Path $explicit "rsxgl\files\x.fcg")) { throw "corpus row resolved wrong: $p" }
    $p = Resolve-StageTreePath -RepoRoot $judged -CorpusRoot $explicit -Rel "build\shader-corpus\th06\y.fcg"
    if ($p -ne (Join-Path $explicit "th06\y.fcg")) { throw "backslashed corpus row resolved wrong: $p" }
    $p = Resolve-StageTreePath -RepoRoot $judged -CorpusRoot $explicit -Rel "tools/rsx-cg-compiler/tests/shaders/fp_x.cg"
    if ($p -ne (Join-Path $judged "tools/rsx-cg-compiler/tests/shaders/fp_x.cg")) { throw "tree row resolved wrong: $p" }
    $p = Resolve-StageTreePath -RepoRoot $judged -CorpusRoot $explicit -Rel "build/shader-corpus-notes/z.fcg"
    if ($p -ne (Join-Path $judged "build/shader-corpus-notes/z.fcg")) { throw "a prefix that merely starts like the corpus dir was treated as corpus: $p" }

    Write-Host "stage-roots-test: ok"
} finally {
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    if (Test-Path -LiteralPath (Join-Path $work "primary")) {
        $null = & git -C (Join-Path $work "primary") worktree remove --force (Join-Path $work "linked") 2>&1
    }
    $ErrorActionPreference = $prevEap
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
