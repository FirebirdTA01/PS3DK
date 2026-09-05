$ErrorActionPreference = "Stop"

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $here "path-pair-corpus-inputs.ps1")

$work = Join-Path $env:TEMP ("sd-reference-tree-test-" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force $work | Out-Null

try {
    New-Item -ItemType Directory -Force (Join-Path $work "tracked") | Out-Null
    Set-Content -LiteralPath (Join-Path $work "tracked\kept.fcg") -Value "void main(out float4 c : COLOR) { c = 1; }" -Encoding Ascii

    $manifest = Join-Path $work "reference-tree.txt"
    Set-Content -LiteralPath $manifest -Value @(
        "# comments and blanks are ignored",
        "",
        "tracked/kept.fcg"
    ) -Encoding Ascii

    $files = @(Get-ReferenceTreeCorpusFiles -Root $work -Manifest $manifest)
    if ($files.Count -ne 1 -or $files[0].RelativePath -ne "tracked/kept.fcg") {
        throw "reference tree manifest mode did not select exactly the listed shader"
    }

    $missing = Join-Path $work "missing-reference-tree.txt"
    Set-Content -LiteralPath $missing -Value "tracked/absent.fcg" -Encoding Ascii
    $missingFailed = $false
    try {
        $null = @(Get-ReferenceTreeCorpusFiles -Root $work -Manifest $missing)
    } catch {
        $missingFailed = ($_.Exception.Message -like "*manifest lists missing shader*")
    }
    if (-not $missingFailed) {
        throw "missing reference tree manifest entry did not fail loudly"
    }

    $line = Format-ReferenceTreeCorpusSummary `
        -CandidateCount 180 `
        -OursRefused 13 `
        -ReferenceRefused 9 `
        -BothRefused 9 `
        -ByteIdentical 29 `
        -PairsStaged 138 `
        -ProbeRows 4
    $expected = "stager: reference tree corpus: 180 candidates, 13 ours-refused, 9 reference-refused, 9 both-refused, 29 byte-identical, 138 pairs staged, 4 probe rows"
    if ($line -ne $expected -or $line.Contains("`n") -or $line.Contains("`r")) {
        throw "reference tree count line wrong: $line"
    }

    $pairsDefault = Get-ReferencePairsPathForStage `
        -ReferencePairs "" `
        -DefaultPath "reference-pairs.txt" `
        -ReferenceTreeCorpus $true `
        -ReferenceCorpus $false `
        -PathPairs $false
    if ($pairsDefault -ne "-") {
        throw "tree-only stage should not implicitly stage curated reference pairs: $pairsDefault"
    }

    $mixedDefault = Get-ReferencePairsPathForStage `
        -ReferencePairs "" `
        -DefaultPath "reference-pairs.txt" `
        -ReferenceTreeCorpus $true `
        -ReferenceCorpus $true `
        -PathPairs $false
    if ($mixedDefault -ne "reference-pairs.txt") {
        throw "mixed reference stage should preserve curated reference-pair default: $mixedDefault"
    }
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host "reference-tree-test: ok"
