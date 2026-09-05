param(
    [Parameter(Mandatory=$true)][string]$CensusCsv,
    [Parameter(Mandatory=$true)][string]$MetricsCsv,
    [Parameter(Mandatory=$true)][string]$TtyLog
)

$ErrorActionPreference = "Stop"

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $here "container-metrics.ps1")

function Require-File([string]$Path, [string]$Label) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Label missing: $Path"
    }
}

function Require-CsvColumns([string]$Path, [string[]]$Columns, [string]$Label) {
    $header = Get-Content -LiteralPath $Path -TotalCount 1
    if (-not $header) {
        throw "$Label has no header"
    }
    $names = @($header -split "," | ForEach-Object { $_.Trim().Trim('"') })
    foreach ($col in $Columns) {
        if ($col -notin $names) {
            throw "$Label missing required column '$col'"
        }
    }
}

function Metric-Int($Value) {
    if ($null -eq $Value -or "$Value" -eq "") { return 0 }
    return [int]$Value
}

Require-File $CensusCsv "census CSV"
Require-File $MetricsCsv "metrics CSV"
Require-File $TtyLog "TTY log"
Require-CsvColumns $CensusCsv @("name", "ours_status", "reference_status", "bucket") "census CSV"
Require-CsvColumns $MetricsCsv @("instruction_delta", "register_delta") "metrics CSV"

$census = @(Import-Csv -LiteralPath $CensusCsv)
$metrics = @(Import-Csv -LiteralPath $MetricsCsv)
$sdiff = @(Parse-SdiffRows (Get-Content -Raw -LiteralPath $TtyLog))

$referenceAcceptedRefusals = @(
    $census | Where-Object {
        $_.reference_status -eq "accept" -and $_.ours_status -ne "accept"
    }
)
$registerBudget = @(
    $referenceAcceptedRefusals | Where-Object { $_.bucket -eq "register_budget" }
).Count
$pixelMismatches = @(
    $sdiff | Where-Object {
        $_.role -notlike "control*" -and $_.status -eq "mismatch"
    }
).Count
$newlyRefusing = @(
    $census | Where-Object {
        $_.reference_status -eq "accept" -and $_.ours_status -eq "new-refuse"
    }
).Count
$worseRegs = @($metrics | Where-Object { (Metric-Int $_.register_delta) -gt 0 }).Count
$worseInstr = @($metrics | Where-Object { (Metric-Int $_.instruction_delta) -gt 0 }).Count

Write-Host ("CURRENT_GAPS|census_rows={0}|accepted_refusals={1}|register_budget={2}|pixel_mismatches={3}|newly_refusing={4}|metrics_worse_regs={5}|metrics_worse_instr={6}" -f `
    $census.Count, $referenceAcceptedRefusals.Count, $registerBudget, $pixelMismatches, $newlyRefusing, $worseRegs, $worseInstr)

$referenceAcceptedRefusals |
    Group-Object bucket |
    Sort-Object Name |
    ForEach-Object {
        $names = ($_.Group | Sort-Object name | ForEach-Object { $_.name }) -join ","
        Write-Host ("GAP_BUCKET|bucket={0}|count={1}|names={2}" -f $_.Name, $_.Count, $names)
    }
