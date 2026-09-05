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
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host "path-pair-corpus-inputs-test: ok"
