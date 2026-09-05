param(
    [Parameter(Mandatory=$true)][string]$CensusCsv,
    [Parameter(Mandatory=$true)][string]$MetricsCsv,
    [Parameter(Mandatory=$true)][string]$TtyLog,
    [Parameter(Mandatory=$true)][string]$StageLog
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

function Get-ShadersExamined([string]$Path) {
    $total = 0
    $matched = $false
    foreach ($line in Get-Content -LiteralPath $Path) {
        if ($line -match '^stager: reference corpus: ([0-9]+) shaders,') {
            $total += [int]$Matches[1]
            $matched = $true
        } elseif ($line -match '^stager: reference tree corpus: ([0-9]+) candidates,') {
            $total += [int]$Matches[1]
            $matched = $true
        } elseif ($line -match '^stager: vp corpus: ([0-9]+) vertex shaders,') {
            $total += [int]$Matches[1]
            $matched = $true
        }
    }
    if (-not $matched) {
        throw "stage log has no reference/vp corpus candidate count: $Path"
    }
    return $total
}

Require-File $CensusCsv "census CSV"
Require-File $MetricsCsv "metrics CSV"
Require-File $TtyLog "TTY log"
Require-File $StageLog "stage log"
Require-CsvColumns $CensusCsv @("name", "ours_status", "reference_status", "bucket") "census CSV"
Require-CsvColumns $MetricsCsv @("instruction_delta", "register_delta") "metrics CSV"

$census = @(Import-Csv -LiteralPath $CensusCsv)
$metrics = @(Import-Csv -LiteralPath $MetricsCsv)
$sdiff = @(Parse-SdiffRows (Get-Content -Raw -LiteralPath $TtyLog))
$shadersExamined = Get-ShadersExamined $StageLog

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

Write-Host ("CURRENT_GAPS|staged_shaders_examined={0}|census_refusal_rows={1}|accepted_refusals={2}|register_budget={3}|pixel_mismatches={4}|newly_refusing={5}|metrics_worse_regs={6}|metrics_worse_instr={7}" -f `
    $shadersExamined, $census.Count, $referenceAcceptedRefusals.Count, $registerBudget, $pixelMismatches, $newlyRefusing, $worseRegs, $worseInstr)

$referenceAcceptedRefusals |
    Group-Object bucket |
    Sort-Object Name |
    ForEach-Object {
        $names = ($_.Group | Sort-Object name | ForEach-Object { $_.name }) -join ","
        Write-Host ("GAP_BUCKET|bucket={0}|count={1}|names={2}" -f $_.Name, $_.Count, $names)
    }
