# Pure lookup shared with boot-sweep; loading this file never starts an emulator.
function Assert-SampleSweepRun([string]$RunDirectory) {
    if (-not $RunDirectory -or
        -not (Test-Path -LiteralPath (Join-Path $RunDirectory 'results.csv') -PathType Leaf) -or
        -not (Test-Path -LiteralPath (Join-Path $RunDirectory 'logs') -PathType Container)) {
        throw 'Specify -sweepDir as one sweep run directory containing results.csv and logs.'
    }
}

function Resolve-SampleSweepArtifact([string]$RunDirectory, [string]$SampleName) {
    Assert-SampleSweepRun $RunDirectory
    if ($SampleName -notmatch '^[A-Za-z0-9_-]+$') { throw "Invalid sample name: $SampleName" }
    $matches = @(Get-ChildItem -LiteralPath $RunDirectory -Directory -ErrorAction Stop |
        Where-Object { $_.Name -match '^b[0-9]+$' } |
        ForEach-Object { Get-ChildItem -LiteralPath $_.FullName -Recurse -File -ErrorAction Stop } |
        Where-Object { $_.Name -ceq "$SampleName.fake.self" })
    if ($matches.Count -gt 1) { throw "Ambiguous artifact: $SampleName in $RunDirectory" }
    if ($matches.Count -eq 1) { return $matches[0] }
    return $null
}
