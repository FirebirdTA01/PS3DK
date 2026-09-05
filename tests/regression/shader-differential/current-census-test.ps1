$ErrorActionPreference = "Stop"

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$script = Join-Path $here "current-census.ps1"
$work = Join-Path $env:TEMP ("sd-current-census-test-" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force $work | Out-Null

function AssertEq($Expected, $Actual, [string]$Label) {
    if ("$Expected" -ne "$Actual") {
        throw "$Label expected '$Expected', got '$Actual'"
    }
}

try {
    Set-Content -LiteralPath (Join-Path $work "reference-corpus-refused.txt") -Value @(
        "# header",
        "test_48_refract|ours|tests/test_48_refract.fcg|1",
        "test_86_profile_restricted|reference|tests/test_86_profile_restricted.fcg|1",
        "both_refuse|ours|tests/both_refuse.fcg|1",
        "both_refuse|reference|tests/both_refuse.fcg|1"
    ) -Encoding Ascii
    Set-Content -LiteralPath (Join-Path $work "vp-corpus-refused.txt") -Value @(
        "# header",
        "th06_v|ours|games/th06_v.vcg|1"
    ) -Encoding Ascii
    Set-Content -LiteralPath (Join-Path $work "reference-tree-corpus-refused.txt") -Value @(
        "# header",
        "tree_only_gap|ours|samples/tree_only_gap.fcg|1"
    ) -Encoding Ascii
    $exclude = Join-Path $work "reference-corpus-exclude.txt"
    Set-Content -LiteralPath $exclude -Value @(
        "# path|board id|path|why",
        "samples/scanlines.fcg|t_de192d41|general|general-path container poisons the sweep"
    ) -Encoding Ascii

    $bucketMap = Join-Path $work "buckets.csv"
    @(
        [pscustomobject]@{ name="test_48_refract"; source=""; bucket="operand_resolution" },
        [pscustomobject]@{ name="th06_v"; source=""; bucket="operand_resolution" },
        [pscustomobject]@{ name="tree_only_gap"; source=""; bucket="reference_tree" },
        [pscustomobject]@{ name=""; source="samples/scanlines.fcg"; bucket="one_off" }
    ) | Export-Csv -NoTypeInformation -Path $bucketMap -Encoding Ascii

    $base = Join-Path $work "base.csv"
    @(
        [pscustomobject]@{ name="th06_v"; profile="sce_vp_rsx"; source="games/th06_v.vcg"; ours_status="accept"; reference_status="accept"; bucket="closed"; rc_ours="0"; rc_reference="0" }
    ) | Export-Csv -NoTypeInformation -Path $base -Encoding Ascii

    $out = Join-Path $work "census.csv"
    $text = powershell -NoProfile -ExecutionPolicy Bypass -File $script -SidecarRoot $work -OutputCsv $out -BucketMapCsv $bucketMap -BaseCensusCsv $base -ExcludeList $exclude 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "current-census failed: $($text | Out-String)"
    }

    $rows = @(Import-Csv -LiteralPath $out)
    AssertEq 6 $rows.Count "row count"

    $refract = $rows | Where-Object { $_.name -eq "test_48_refract" }
    AssertEq "backend-refuse" $refract.ours_status "ours refusal"
    AssertEq "accept" $refract.reference_status "reference accepted implied by one-sided ours refusal"
    AssertEq "operand_resolution" $refract.bucket "bucket map"
    AssertEq "sce_fp_rsx" $refract.profile "FP profile"

    $th06 = $rows | Where-Object { $_.name -eq "th06_v" }
    AssertEq "new-refuse" $th06.ours_status "base accepted but current refuses"
    AssertEq "sce_vp_rsx" $th06.profile "VP profile"

    $treeOnly = $rows | Where-Object { $_.name -eq "tree_only_gap" }
    AssertEq "backend-refuse" $treeOnly.ours_status "reference-tree ours refusal"
    AssertEq "accept" $treeOnly.reference_status "reference-tree reference accepted implied by one-sided ours refusal"
    AssertEq "reference_tree" $treeOnly.bucket "reference-tree bucket"
    AssertEq "sce_fp_rsx" $treeOnly.profile "reference-tree FP profile"

    $test86 = $rows | Where-Object { $_.name -eq "test_86_profile_restricted" }
    AssertEq "accept" $test86.ours_status "ours accepted reference-only refusal"
    AssertEq "backend-refuse" $test86.reference_status "reference refusal"

    $both = $rows | Where-Object { $_.name -eq "both_refuse" }
    AssertEq "backend-refuse" $both.ours_status "both refuse ours"
    AssertEq "backend-refuse" $both.reference_status "both refuse reference"

    $scanlines = $rows | Where-Object { $_.source -eq "samples/scanlines.fcg" }
    AssertEq "excluded-poison" $scanlines.ours_status "excluded poison status"
    AssertEq "accept" $scanlines.reference_status "excluded poison reference status"
    AssertEq "one_off" $scanlines.bucket "excluded poison bucket"

    $emptyRoot = Join-Path $work "empty"
    New-Item -ItemType Directory -Force $emptyRoot | Out-Null
    $emptyOut = Join-Path $work "empty.csv"
    $emptyText = powershell -NoProfile -ExecutionPolicy Bypass -File $script -SidecarRoot $emptyRoot -OutputCsv $emptyOut 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "empty current-census failed: $($emptyText | Out-String)"
    }
    $header = Get-Content -LiteralPath $emptyOut -TotalCount 1
    AssertEq '"name","profile","source","ours_status","reference_status","bucket","rc_ours","rc_reference"' $header "empty census header"
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host "current-census-test: ok"
