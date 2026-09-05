$ErrorActionPreference = "Stop"

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$script = Join-Path $here "current-gap-summary.ps1"
$work = Join-Path $env:TEMP ("sd-current-gap-summary-test-" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force $work | Out-Null

function Assert-Contains([string]$Haystack, [string]$Needle, [string]$Label) {
    if (-not $Haystack.Contains($Needle)) {
        throw "$Label missing '$Needle' in: $Haystack"
    }
}

function Invoke-GapSummary([string]$Census, [string]$Metrics, [string]$Tty, [string]$StageLog) {
    $output = powershell -NoProfile -ExecutionPolicy Bypass -File $script -CensusCsv $Census -MetricsCsv $Metrics -TtyLog $Tty -StageLog $StageLog 2>&1
    return [pscustomobject]@{
        ExitCode = $LASTEXITCODE
        Text = ($output | Out-String)
    }
}

try {
    $oldErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $missing = powershell -NoProfile -ExecutionPolicy Bypass -File $script -CensusCsv (Join-Path $work "missing.csv") -MetricsCsv (Join-Path $work "missing-metrics.csv") -TtyLog (Join-Path $work "missing.log") -StageLog (Join-Path $work "missing-stage.log") 2>&1
    $missingExitCode = $LASTEXITCODE
    $ErrorActionPreference = $oldErrorActionPreference
    if ($missingExitCode -eq 0) {
        throw "missing inputs returned success: $($missing | Out-String)"
    }

    $census = Join-Path $work "census.csv"
    @(
        [pscustomobject]@{ name="test_48_refract"; profile="sce_fp_rsx"; source="a.fcg"; ours_status="backend-refuse"; reference_status="accept"; bucket="operand_resolution" },
        [pscustomobject]@{ name="test_79_centroid_interpolation"; profile="sce_fp_rsx"; source="b.fcg"; ours_status="register-budget"; reference_status="accept"; bucket="register_budget" },
        [pscustomobject]@{ name="new_refuse"; profile="sce_fp_rsx"; source="c.fcg"; ours_status="new-refuse"; reference_status="accept"; bucket="one_off" },
        [pscustomobject]@{ name="reference_refuses_too"; profile="sce_fp_rsx"; source="d.fcg"; ours_status="backend-refuse"; reference_status="backend-refuse"; bucket="profile_restriction" },
        [pscustomobject]@{ name="already_ok"; profile="sce_fp_rsx"; source="e.fcg"; ours_status="accept"; reference_status="accept"; bucket="closed" }
    ) | Export-Csv -NoTypeInformation -Path $census -Encoding Ascii

    $metrics = Join-Path $work "metrics.csv"
    @(
        [pscustomobject]@{ name="test_48_refract"; instruction_delta="4"; register_delta="1"; byte_identical="False"; pixel_status="unknown" },
        [pscustomobject]@{ name="already_ok"; instruction_delta="-1"; register_delta="0"; byte_identical="False"; pixel_status="identical" }
    ) | Export-Csv -NoTypeInformation -Path $metrics -Encoding Ascii

    $tty = Join-Path $work "tty.log"
    @(
        "SDIFF|tier=B|role=control-mismatch|shader=ctrl|compiler=ab|uniform_set=0|target=emulator|status=mismatch|max_delta=1|diff_pixels=1|total_pixels=4096|diagnostic=control|elapsed_ms=1|artifact=-",
        "SDIFF|tier=B|role=reference|shader=bad_pixels|compiler=ab|uniform_set=0|target=emulator|status=mismatch|max_delta=9|diff_pixels=64|total_pixels=4096|diagnostic=real|elapsed_ms=1|artifact=-"
    ) | Set-Content -Path $tty -Encoding Ascii

    $stageLog = Join-Path $work "stage.log"
    @(
        "stager: reference corpus: 117 shaders, 97 pairs staged, 7 byte-identical skipped, ours refused 12, reference refused 1, 1 excluded, 11 reference-only probe rows (sidecar: reference-corpus-refused.txt)",
        "stager: path-pair corpus (gate 1): 180 shaders from manifest path-pair-corpus-manifest.txt, 0 excluded, 103 legacy-refused (out of scope), 0 GENERAL-REFUSED (gate failures), 0 reference-refused (unoracled), 29 byte-identical legacy/general, 48 pairs staged (0 under set 0 for a file-scope const)",
        "stager: vp corpus: 18 vertex shaders, 14 pairs staged, 2 byte-identical skipped, ours refused 2, reference refused 0, 0 excluded, 0 under set 0 (sidecar: vp-corpus-refused.txt)",
        "stager: vp path pairs (gate 5): 104 candidates (16 curated from vp-path-pairs.txt + 88 from corpus, 0 already curated and counted once), 0 legacy-refused (out of scope), 0 GENERAL-REFUSED (gate failures), 0 reference-refused (unoracled), 0 byte-identical legacy/general, 104 pairs staged (sidecar: vp-path-pair-refused.txt)"
    ) | Set-Content -Path $stageLog -Encoding Ascii

    $result = Invoke-GapSummary $census $metrics $tty $stageLog
    if ($result.ExitCode -ne 0) {
        throw "current-gap-summary failed unexpectedly: $($result.Text)"
    }
    Assert-Contains $result.Text "CURRENT_GAPS|staged_shaders_examined=135|census_refusal_rows=5|accepted_refusals=3|register_budget=1|pixel_mismatches=1|newly_refusing=1|metrics_worse_regs=1|metrics_worse_instr=1" "summary"
    Assert-Contains $result.Text "GAP_BUCKET|bucket=operand_resolution|count=1|names=test_48_refract" "operand bucket"
    Assert-Contains $result.Text "GAP_BUCKET|bucket=register_budget|count=1|names=test_79_centroid_interpolation" "register bucket"

    @(
        [pscustomobject]@{ name="only_one"; profile="sce_fp_rsx"; source="a.fcg"; ours_status="backend-refuse"; reference_status="accept"; bucket="one_off" }
    ) | Export-Csv -NoTypeInformation -Path $census -Encoding Ascii
    $changed = Invoke-GapSummary $census $metrics $tty $stageLog
    Assert-Contains $changed.Text "CURRENT_GAPS|staged_shaders_examined=135|census_refusal_rows=1|accepted_refusals=1|" "changed census summary"

    Set-Content -LiteralPath $stageLog -Value "stager: reference tree corpus: 180 candidates, 13 ours-refused, 9 reference-refused, 9 both-refused, 29 byte-identical, 138 pairs staged, 4 probe rows" -Encoding Ascii
    $treeOnly = Invoke-GapSummary $census $metrics $tty $stageLog
    Assert-Contains $treeOnly.Text "CURRENT_GAPS|staged_shaders_examined=180|census_refusal_rows=1|accepted_refusals=1|" "reference-tree census summary"

    Set-Content -LiteralPath $census -Value '"name","profile","source","ours_status","reference_status","bucket","rc_ours","rc_reference"' -Encoding Ascii
    Set-Content -LiteralPath $stageLog -Value "stager: reference corpus: 0 shaders, 0 pairs staged, 0 byte-identical skipped, ours refused 0, reference refused 0, 0 excluded, 0 reference-only probe rows (sidecar: reference-corpus-refused.txt)" -Encoding Ascii
    $empty = Invoke-GapSummary $census $metrics $tty $stageLog
    Assert-Contains $empty.Text "CURRENT_GAPS|staged_shaders_examined=0|census_refusal_rows=0|accepted_refusals=0|register_budget=0|" "empty census summary"
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host "current-gap-summary-test: ok"
