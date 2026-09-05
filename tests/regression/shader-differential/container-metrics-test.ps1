$ErrorActionPreference = "Stop"

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $here "container-metrics.ps1")

$work = Join-Path $env:TEMP ("sd-container-metrics-test-" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force $work | Out-Null

function Write-U32BE([System.Collections.Generic.List[byte]]$bytes, [uint32]$value) {
    $bytes.Add([byte](($value -shr 24) -band 0xff))
    $bytes.Add([byte](($value -shr 16) -band 0xff))
    $bytes.Add([byte](($value -shr 8) -band 0xff))
    $bytes.Add([byte]($value -band 0xff))
}

function Write-U16BE([System.Collections.Generic.List[byte]]$bytes, [uint16]$value) {
    $bytes.Add([byte](($value -shr 8) -band 0xff))
    $bytes.Add([byte]($value -band 0xff))
}

function New-Container([string]$path, [uint32]$profile, [uint32]$instructionCount, [uint32]$registerCount) {
    $programOffset = [uint32]0x20
    $bytes = [System.Collections.Generic.List[byte]]::new()
    Write-U32BE $bytes $profile
    Write-U32BE $bytes 0
    Write-U32BE $bytes 0
    Write-U32BE $bytes 0
    Write-U32BE $bytes 0x20
    Write-U32BE $bytes $programOffset
    Write-U32BE $bytes 0
    Write-U32BE $bytes 0
    if ($profile -eq 0x00001b5c) {
        Write-U32BE $bytes $instructionCount
        Write-U32BE $bytes 0
        Write-U32BE $bytes 0
        Write-U16BE $bytes 0
        Write-U16BE $bytes 0
        Write-U16BE $bytes 0
        $bytes.Add([byte]$registerCount)
        $bytes.Add(0)
        $bytes.Add(0)
        $bytes.Add(0)
    } else {
        Write-U32BE $bytes $instructionCount
        Write-U32BE $bytes 0
        Write-U32BE $bytes $registerCount
        Write-U32BE $bytes 0
        Write-U32BE $bytes 0
        Write-U32BE $bytes 0
    }
    [System.IO.File]::WriteAllBytes($path, $bytes.ToArray())
}

try {
    $fpA = Join-Path $work "a.fpo"
    $fpB = Join-Path $work "b.fpo"
    $vp = Join-Path $work "a.vpo"
    New-Container $fpA 0x00001b5c 37 3
    New-Container $fpB 0x00001b5c 21 2
    New-Container $vp  0x00001b5b 11 5

    $fpMetrics = Read-ShaderContainerMetrics $fpA
    if ($fpMetrics.profile -ne "sce_fp_rsx" -or
        $fpMetrics.instruction_count -ne 37 -or
        $fpMetrics.register_count -ne 3) {
        throw "FP metrics wrong: $($fpMetrics | ConvertTo-Json -Compress)"
    }

    $vpMetrics = Read-ShaderContainerMetrics $vp
    if ($vpMetrics.profile -ne "sce_vp_rsx" -or
        $vpMetrics.instruction_count -ne 11 -or
        $vpMetrics.register_count -ne 5) {
        throw "VP metrics wrong: $($vpMetrics | ConvertTo-Json -Compress)"
    }

    $rows = @()
    Add-ContainerMetricsRow ([ref]$rows) -Name "asin" -Role "reference" `
        -Profile "sce_fp_rsx" -Source "shader.cg" -UniformSet "0" `
        -OursPath $fpA -ReferencePath $fpB -ByteIdentical:$false -Staged:$true

    if ($rows.Count -ne 1) { throw "expected one metrics row" }
    $row = $rows[0]
    if ($row.ours_instruction_count -ne 37 -or
        $row.reference_instruction_count -ne 21 -or
        $row.instruction_delta -ne 16 -or
        $row.ours_register_count -ne 3 -or
        $row.reference_register_count -ne 2 -or
        $row.register_delta -ne 1) {
        throw "comparison row wrong: $($row | ConvertTo-Json -Compress)"
    }

    $summary = Get-ContainerMetricsSummary $rows
    if ($summary.Compared -ne 1 -or
        $summary.InstructionMismatches -ne 1 -or
        $summary.RegisterMismatches -ne 1 -or
        $summary.BothMismatches -ne 1) {
        throw "summary wrong: $($summary | ConvertTo-Json -Compress)"
    }

    $sdiffText = "noise`nSDIFF|tier=B|role=reference|shader=asin|compiler=ab|uniform_set=0|target=emulator|status=identical|max_delta=0|diff_pixels=0|total_pixels=4096|diagnostic=OK|elapsed_ms=1|artifact=-`n"
    $joined = Join-ContainerMetricsWithSdiff $rows (Parse-SdiffRows $sdiffText)
    if ($joined[0].pixel_status -ne "identical") {
        throw "pixel status was not joined from SDIFF row"
    }
    $joinedSummary = Get-ContainerMetricsSummary $joined
    if ($joinedSummary.PixelProofRows -ne 1) {
        throw "summary did not report the identical-pixel/register-drift proof row"
    }

    $csv = Join-Path $work "metrics.csv"
    $joined | Export-Csv -NoTypeInformation -Path $csv -Encoding Ascii
    $importedSummary = Get-ContainerMetricsSummary @(Import-Csv -LiteralPath $csv)
    if ($importedSummary.PixelProofRows -ne 1 -or
        $importedSummary.PixelProofCandidates -ne 1) {
        throw "imported CSV metrics did not preserve proof/candidate counts"
    }

    $baseline = @(
        [pscustomobject]@{
            name = "asin"
            role = "reference"
            profile = "sce_fp_rsx"
            source = "shader.cg"
            uniform_set = "0"
            instruction_delta = "16"
            register_delta = "1"
        }
    )
    $gate = Get-ContainerMetricsGateSummary $joined $baseline
    if ($gate.Status -ne "ok" -or
        $gate.CurrentRows -ne 1 -or
        $gate.BaselineRows -ne 1 -or
        $gate.BaselineRegressions -ne 0 -or
        $gate.MissingBaselineRows -ne 0) {
        throw "baseline-equal gate summary wrong: $($gate | ConvertTo-Json -Compress)"
    }

    $regressed = @(
        [pscustomobject]@{
            name = "asin"
            role = "reference"
            profile = "sce_fp_rsx"
            source = "shader.cg"
            uniform_set = "0"
            instruction_delta = "17"
            register_delta = "2"
        }
    )
    $badGate = Get-ContainerMetricsGateSummary $regressed $baseline
    if ($badGate.Status -ne "fail" -or
        $badGate.BaselineRegressions -ne 1 -or
        $badGate.WorstInstructionRegression -ne 1 -or
        $badGate.WorstRegisterRegression -ne 1) {
        throw "baseline-regressed gate summary wrong: $($badGate | ConvertTo-Json -Compress)"
    }

    $newRows = @(
        [pscustomobject]@{
            name = "asin"
            role = "reference"
            profile = "sce_fp_rsx"
            source = "shader.cg"
            uniform_set = "0"
            instruction_delta = "16"
            register_delta = "1"
        },
        [pscustomobject]@{
            name = "new_acceptance"
            role = "reference"
            profile = "sce_fp_rsx"
            source = "new.cg"
            uniform_set = "auto"
            instruction_delta = "12"
            register_delta = "3"
        }
    )
    $missingGate = Get-ContainerMetricsGateSummary $newRows $baseline
    if ($missingGate.Status -ne "ok" -or
        $missingGate.ComparedBaselineRows -ne 1 -or
        $missingGate.MissingBaselineRows -ne 1 -or
        $missingGate.BaselineRegressions -ne 0) {
        throw "missing-baseline row should be reported but not failed: $($missingGate | ConvertTo-Json -Compress)"
    }

    $allMissing = @(
        [pscustomobject]@{
            name = "new_mode"
            role = "reference-tree-corpus"
            profile = "sce_fp_rsx"
            source = "new-mode.cg"
            uniform_set = "auto"
            instruction_delta = "4"
            register_delta = "1"
        }
    )
    $noCoverageGate = Get-ContainerMetricsGateSummary $allMissing $baseline
    if ($noCoverageGate.Status -ne "no-coverage" -or
        $noCoverageGate.ComparedBaselineRows -ne 0 -or
        $noCoverageGate.MissingBaselineRows -ne 1 -or
        $noCoverageGate.BaselineRegressions -ne 0) {
        throw "all-missing baseline rows should report no-coverage: $($noCoverageGate | ConvertTo-Json -Compress)"
    }

    $baselinePath = Join-Path $work "container-metrics-baseline.csv"
    $baseline | Export-Csv -NoTypeInformation -Path $baselinePath -Encoding Ascii
    $gateOutput = Write-ContainerMetricsGateReport $joined $baselinePath 6>$null
    if ($gateOutput.Mode -ne "fail-by-default" -or
        $gateOutput.ShouldFail -or
        $gateOutput.Summary.Status -ne "ok") {
        throw "baseline-present gate report should be fail-by-default and ok: $($gateOutput | ConvertTo-Json -Compress -Depth 4)"
    }

    $badOutput = Write-ContainerMetricsGateReport $regressed $baselinePath 6>$null
    if ($badOutput.Mode -ne "fail-by-default" -or
        -not $badOutput.ShouldFail -or
        $badOutput.Summary.BaselineRegressions -ne 1) {
        throw "baseline-present regression should fail by default: $($badOutput | ConvertTo-Json -Compress -Depth 4)"
    }

    $reportOnlyOutput = Write-ContainerMetricsGateReport $regressed $baselinePath -ReportOnly 6>$null
    if ($reportOnlyOutput.Mode -ne "report-only" -or
        $reportOnlyOutput.ShouldFail -or
        $reportOnlyOutput.Summary.Status -ne "fail") {
        throw "MetricsReportOnly should report regression without failing: $($reportOnlyOutput | ConvertTo-Json -Compress -Depth 4)"
    }

    $noCoverageOutput = Write-ContainerMetricsGateReport $allMissing $baselinePath 6>$null
    if ($noCoverageOutput.Mode -ne "fail-by-default" -or
        -not $noCoverageOutput.ShouldFail -or
        $noCoverageOutput.Summary.Status -ne "no-coverage") {
        throw "baseline-present all-missing rows should fail with no-coverage: $($noCoverageOutput | ConvertTo-Json -Compress -Depth 4)"
    }

    $noCoverageReportOnly = Write-ContainerMetricsGateReport $allMissing $baselinePath -ReportOnly 6>$null
    if ($noCoverageReportOnly.Mode -ne "report-only" -or
        $noCoverageReportOnly.ShouldFail -or
        $noCoverageReportOnly.Summary.Status -ne "no-coverage") {
        throw "MetricsReportOnly should report no-coverage without failing: $($noCoverageReportOnly | ConvertTo-Json -Compress -Depth 4)"
    }

    $absentOutput = Write-ContainerMetricsGateReport $joined (Join-Path $work "missing-baseline.csv") 6>$null
    if ($absentOutput.Mode -ne "report-only" -or
        $absentOutput.ShouldFail -or
        $absentOutput.Summary.BaselineRows -ne 0) {
        throw "missing baseline should be report-only: $($absentOutput | ConvertTo-Json -Compress -Depth 4)"
    }

    $badBaselinePath = Join-Path $work "bad-baseline.csv"
    Set-Content -LiteralPath $badBaselinePath -Value '"name","role","profile","source","uniform_set","instruction_delta"' -Encoding Ascii
    $badBaselineFailed = $false
    try {
        Write-ContainerMetricsGateReport $joined $badBaselinePath | Out-Null
    } catch {
        $badBaselineFailed = $_.Exception.Message.Contains("baseline CSV missing required column")
    }
    if (-not $badBaselineFailed) {
        throw "malformed baseline did not fail loudly"
    }
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host "container-metrics-test: ok"
