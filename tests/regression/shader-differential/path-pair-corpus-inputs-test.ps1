$ErrorActionPreference = "Stop"

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $here "path-pair-corpus-inputs.ps1")

$work = Join-Path $env:TEMP ("sd-path-pair-inputs-test-" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force $work | Out-Null

try {
    New-Item -ItemType Directory -Force (Join-Path $work "tracked") | Out-Null
    New-Item -ItemType Directory -Force (Join-Path $work "scratch") | Out-Null
    Set-Content -LiteralPath (Join-Path $work "tracked\kept.fcg") -Value "void main(out float4 c : COLOR) { c = 1; }" -Encoding Ascii
    Set-Content -LiteralPath (Join-Path $work "scratch\leak.fcg") -Value "void main(out float4 c : COLOR) { c = 0; }" -Encoding Ascii

    $manifest = Join-Path $work "path-pair-corpus.txt"
    Set-Content -LiteralPath $manifest -Value @(
        "# comments and blanks are ignored",
        "",
        "tracked/kept.fcg"
    ) -Encoding Ascii

    $manifestFiles = @(Get-PathPairCorpusFiles -Root $work -Manifest $manifest)
    if ($manifestFiles.Count -ne 1 -or $manifestFiles[0].RelativePath -ne "tracked/kept.fcg") {
        throw "manifest mode did not select exactly the listed shader"
    }

    $walkFiles = @(Get-PathPairCorpusFiles -Root $work)
    if (@($walkFiles | Where-Object { $_.RelativePath -eq "scratch/leak.fcg" }).Count -ne 1) {
        throw "walk mode no longer sees ordinary corpus files: $(@($walkFiles | ForEach-Object { $_.RelativePath }) -join ', ')"
    }

    $missing = Join-Path $work "missing.txt"
    Set-Content -LiteralPath $missing -Value "tracked/absent.fcg" -Encoding Ascii
    $missingFailed = $false
    try {
        $null = @(Get-PathPairCorpusFiles -Root $work -Manifest $missing)
    } catch {
        $missingFailed = ($_.Exception.Message -like "*manifest lists missing shader*")
    }
    if (-not $missingFailed) {
        throw "missing manifest entry did not fail loudly"
    }

    # The pairs-list fallback: only the DEFAULT list falls back to the earlier
    # file name beside it; a list named explicitly is never replaced.
    $rigOld = Join-Path $work "rig-old"
    New-Item -ItemType Directory -Force $rigOld | Out-Null
    $legacy = Join-Path $rigOld "reference-pairs.txt"
    Set-Content -LiteralPath $legacy -Value "# earlier list" -Encoding Ascii
    $defaultOld = Join-Path $rigOld "established-pairs.txt"
    $got = Resolve-EstablishedPairsFile $defaultOld $defaultOld
    if ($got -ne $legacy) { throw "a missing default list did not fall back to the earlier file: $got" }

    $custom = Join-Path $rigOld "my-pairs.txt"
    $got = Resolve-EstablishedPairsFile $custom $defaultOld
    if ($got -ne $custom) { throw "a missing EXPLICIT list was replaced by the earlier file: $got" }
    $elsewhere = Join-Path $work "scratch\established-pairs.txt"
    $got = Resolve-EstablishedPairsFile $elsewhere $defaultOld
    if ($got -ne $elsewhere) { throw "a missing explicit list of the default's NAME in another directory was replaced: $got" }

    Set-Content -LiteralPath $custom -Value "# custom list" -Encoding Ascii
    $got = Resolve-EstablishedPairsFile $custom $defaultOld
    if ($got -ne $custom) { throw "an existing explicit list was not returned as asked: $got" }

    Set-Content -LiteralPath $defaultOld -Value "# new list" -Encoding Ascii
    $got = Resolve-EstablishedPairsFile $defaultOld $defaultOld
    if ($got -ne $defaultOld) { throw "the default list was not preferred over the earlier file: $got" }

    $rigNew = Join-Path $work "rig-none"
    New-Item -ItemType Directory -Force $rigNew | Out-Null
    $defaultNone = Join-Path $rigNew "established-pairs.txt"
    $got = Resolve-EstablishedPairsFile $defaultNone $defaultNone
    if ($got -ne $defaultNone) { throw "a missing default with no earlier file did not stay as asked: $got" }
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host "path-pair-corpus-inputs-test: ok"
