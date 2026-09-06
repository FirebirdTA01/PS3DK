$ErrorActionPreference = "Stop"

function Test-PathPairCorpusShaderName([string]$name) {
    return ($name -like "*.fcg") -or ($name -like "*_f.cg")
}

function Get-PathPairCorpusFiles([string]$Root, [string]$Manifest = "") {
    if (-not (Test-Path -LiteralPath $Root -PathType Container)) {
        throw "path-pair corpus root not a directory: $Root"
    }
    $rootPath = [System.IO.Path]::GetFullPath((Get-Item -LiteralPath $Root).FullName).TrimEnd('\')

    if ($Manifest) {
        if (-not (Test-Path -LiteralPath $Manifest -PathType Leaf)) {
            throw "path-pair corpus manifest not found: $Manifest"
        }
        $rows = @()
        $seen = @{}
        foreach ($line in (Get-Content -LiteralPath $Manifest)) {
            $rel = $line.Trim()
            if (-not $rel -or $rel.StartsWith("#")) { continue }
            $rel = $rel.Replace('\', '/')
            $parts = @($rel -split '/')
            if ([System.IO.Path]::IsPathRooted($rel) -or ($parts | Where-Object { $_ -eq ".." }).Count -ne 0) {
                throw "path-pair corpus manifest path must be root-relative: $rel"
            }
            if ($seen.ContainsKey($rel)) {
                throw "path-pair corpus manifest lists duplicate shader: $rel"
            }
            $seen[$rel] = 1
            if (-not (Test-PathPairCorpusShaderName ([System.IO.Path]::GetFileName($rel)))) {
                throw "path-pair corpus manifest lists non-fragment shader: $rel"
            }
            $full = [System.IO.Path]::GetFullPath((Join-Path $rootPath ($rel -replace '/', '\')))
            $rootPrefix = $rootPath.TrimEnd('\') + '\'
            if (-not $full.StartsWith($rootPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
                throw "path-pair corpus manifest path escapes root: $rel"
            }
            if (-not (Test-Path -LiteralPath $full -PathType Leaf)) {
                throw "path-pair corpus manifest lists missing shader: $rel"
            }
            $rows += [pscustomobject]@{
                File = Get-Item -LiteralPath $full
                RelativePath = $rel
            }
        }
        if ($rows.Count -eq 0) {
            throw "path-pair corpus manifest is empty: $Manifest"
        }
        return $rows
    }

    return @(Get-ChildItem -LiteralPath $rootPath -Recurse -File |
        Where-Object { Test-PathPairCorpusShaderName $_.Name } |
        Where-Object {
            $r = $_.FullName.Substring($rootPath.Length + 1).Replace('\', '/')
            -not ($r.StartsWith('build/') -or $r.Contains('/_work/') -or $r.StartsWith('_work/'))
        } |
        Sort-Object FullName |
        ForEach-Object {
            [pscustomobject]@{
                File = $_
                RelativePath = $_.FullName.Substring($rootPath.Length + 1).Replace('\', '/')
            }
        })
}

function Get-ReferenceTreeCorpusFiles([string]$Root, [string]$Manifest = "") {
    return @(Get-PathPairCorpusFiles -Root $Root -Manifest $Manifest)
}

function Format-ReferenceTreeCorpusSummary(
    [int]$CandidateCount,
    [int]$OursRefused,
    [int]$ReferenceRefused,
    [int]$BothRefused,
    [int]$ByteIdentical,
    [int]$PairsStaged,
    [int]$ProbeRows
) {
    return ("stager: reference tree corpus: {0} candidates, {1} ours-refused, {2} reference-refused, {3} both-refused, {4} byte-identical, {5} pairs staged, {6} probe rows" -f `
        $CandidateCount, $OursRefused, $ReferenceRefused, $BothRefused, $ByteIdentical, $PairsStaged, $ProbeRows)
}

function Get-ReferencePairsPathForStage(
    [string]$ReferencePairs,
    [string]$DefaultPath,
    [bool]$ReferenceTreeCorpus,
    [bool]$ReferenceCorpus,
    [bool]$PathPairs
) {
    if ($ReferencePairs) { return $ReferencePairs }
    if ($ReferenceTreeCorpus -and -not ($ReferenceCorpus -or $PathPairs)) { return "-" }
    return $DefaultPath
}

# ---------------------------------------------------------------------------
# Stage roots (t_b1269234).  The stager has THREE roots, and they used to be
# one:
#
#   ScriptTree  the tree the stager CODE lives in - helpers, the sd_*
#               instruments whose verdicts the guest has coded in.
#   RepoRoot    the tree the stage JUDGES: every repo-relative shader in the
#               curated lists, the lists themselves, the tracked-tree
#               manifest, the exclude list, the metrics baseline.  Defaults
#               to ScriptTree; -RepoRoot points it at a worktree.
#   CorpusRoot  the fetched community corpus (build\shader-corpus).  It is a
#               manifest-pinned EXTERNAL fetch that lives beside the primary
#               checkout's .git, so a linked worktree normally has none:
#               explicit -ReferenceCorpusDir, else the judged tree's own
#               fetch, else the primary checkout's (through git's common
#               dir, which is the primary worktree's .git), else the judged
#               tree's path anyway so a corpus row fails as "shader not
#               found" under the tree it named.
#
# Conflating them meant a branch fixture could not be judged from its own
# branch, and a same-named file in the shared tree could be judged instead
# and called a verdict.  Every resolved root carries a note saying where it
# came from; the stager prints them at the top of every stage.
function Resolve-StageRoots(
    [string]$ScriptDir,
    [string]$RepoRoot = "",
    [string]$ReferenceCorpusDir = ""
) {
    $scriptTree = (Resolve-Path -LiteralPath (Join-Path $ScriptDir "..\..\..")).Path.TrimEnd('\')
    if ($RepoRoot) {
        if (-not (Test-Path -LiteralPath $RepoRoot -PathType Container)) {
            throw "-RepoRoot is not a directory: $RepoRoot"
        }
        $repoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path.TrimEnd('\')
    } else {
        $repoRoot = $scriptTree
    }
    $rig = Join-Path $repoRoot "tests\regression\shader-differential"
    # A root without the rig's curated list is not a tree this stager can
    # judge; a typo must not fall through to staging nothing from the wrong
    # place.
    if (-not (Test-Path -LiteralPath (Join-Path $rig "reference-pairs.txt") -PathType Leaf)) {
        throw "-RepoRoot does not hold a shader-differential rig (no tests\regression\shader-differential\reference-pairs.txt under $repoRoot)"
    }

    $corpusRoot = ""
    $corpusNote = ""
    if ($ReferenceCorpusDir) {
        if (-not (Test-Path -LiteralPath $ReferenceCorpusDir -PathType Container)) {
            throw "-ReferenceCorpusDir is not a directory: $ReferenceCorpusDir"
        }
        $corpusRoot = (Resolve-Path -LiteralPath $ReferenceCorpusDir).Path.TrimEnd('\')
        $corpusNote = "-ReferenceCorpusDir"
    } else {
        $ownCorpus = Join-Path $repoRoot "build\shader-corpus"
        if (Test-Path -LiteralPath $ownCorpus -PathType Container) {
            $corpusRoot = $ownCorpus
            $corpusNote = "the judged tree's own fetch"
        } else {
            # git's stderr under "Stop" would be a terminating error; a tree
            # that is not a repository is an ordinary outcome here.
            $prevEap = $ErrorActionPreference
            $ErrorActionPreference = "Continue"
            $commonDir = @(& git -C $repoRoot rev-parse --path-format=absolute --git-common-dir 2>$null) | Select-Object -First 1
            $ErrorActionPreference = $prevEap
            if ($commonDir -and $LASTEXITCODE -eq 0) {
                $primary = Split-Path -Parent (([string]$commonDir -replace '/', '\').TrimEnd('\'))
                $primaryCorpus = Join-Path $primary "build\shader-corpus"
                if (Test-Path -LiteralPath $primaryCorpus -PathType Container) {
                    $corpusRoot = $primaryCorpus
                    $corpusNote = "the primary checkout's fetch; the judged tree has none"
                }
            }
            if (-not $corpusRoot) {
                $corpusRoot = $ownCorpus
                $corpusNote = "NOT FETCHED anywhere; corpus rows and sweeps cannot resolve"
            }
        }
    }

    return [pscustomobject]@{
        ScriptTree = $scriptTree
        RepoRoot   = $repoRoot
        Rig        = $rig
        CorpusRoot = $corpusRoot
        CorpusNote = $corpusNote
    }
}

# The two provenance lines every stage prints first, so a log says which
# tree and which corpus it judged.
function Format-StageRootsBanner($Roots) {
    $tree = if ($Roots.RepoRoot.Equals($Roots.ScriptTree, [System.StringComparison]::OrdinalIgnoreCase)) {
        "stager: tree = $($Roots.RepoRoot)"
    } else {
        "stager: tree = $($Roots.RepoRoot) (judged tree; the stager code runs from $($Roots.ScriptTree))"
    }
    return @($tree, "stager: corpus = $($Roots.CorpusRoot) ($($Roots.CorpusNote))")
}

# A repo-relative path from a curated list, resolved against the tree it
# belongs to: build/shader-corpus/<x> is corpus content and goes through
# the corpus root; everything else is the judged tree's own.  The match is
# on the LITERAL prefix, so a row spelled ./build/shader-corpus/... would
# route to the judged tree - no list spells one that way, and the lists
# are the place to keep it so.
function Resolve-StageTreePath([string]$RepoRoot, [string]$CorpusRoot, [string]$Rel) {
    $n = $Rel -replace '\\', '/'
    if ($n -match '^build/shader-corpus/(.+)$') {
        return Join-Path $CorpusRoot ($Matches[1] -replace '/', '\')
    }
    return Join-Path $RepoRoot $Rel
}
