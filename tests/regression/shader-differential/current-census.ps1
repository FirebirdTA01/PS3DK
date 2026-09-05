param(
    [Parameter(Mandatory=$true)][string]$SidecarRoot,
    [Parameter(Mandatory=$true)][string]$OutputCsv,
    [string]$BucketMapCsv = "",
    [string]$BaseCensusCsv = "",
    [string]$ExcludeList = ""
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $SidecarRoot -PathType Container)) {
    throw "sidecar root missing: $SidecarRoot"
}

function Census-Key([string]$Profile, [string]$Source) {
    return "$Profile|$Source"
}

function Read-BucketMap([string]$Path) {
    $byName = @{}
    $bySource = @{}
    if (-not $Path) {
        return [pscustomobject]@{ ByName = $byName; BySource = $bySource }
    }
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "bucket map missing: $Path"
    }
    foreach ($row in @(Import-Csv -LiteralPath $Path)) {
        if ($row.source) { $bySource[$row.source] = $row.bucket }
        if ($row.name) { $byName[$row.name] = $row.bucket }
    }
    return [pscustomobject]@{ ByName = $byName; BySource = $bySource }
}

function Read-BaseAccepted([string]$Path) {
    $accepted = @{}
    if (-not $Path) { return $accepted }
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "base census missing: $Path"
    }
    foreach ($row in @(Import-Csv -LiteralPath $Path)) {
        if ($row.ours_status -eq "accept" -and $row.reference_status -eq "accept") {
            $accepted[(Census-Key $row.profile $row.source)] = $true
        }
    }
    return $accepted
}

function Add-RefusalRows([hashtable]$Rows, [string]$Path, [string]$Profile) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return }
    foreach ($line in Get-Content -LiteralPath $Path) {
        $trim = $line.Trim()
        if (-not $trim -or $trim.StartsWith("#")) { continue }
        $parts = @($trim -split "\|", 4)
        if ($parts.Count -ne 4) {
            throw "malformed refusal row in ${Path}: $line"
        }
        $name = $parts[0]
        $side = $parts[1]
        $source = $parts[2]
        $rc = $parts[3]
        if ($side -notin @("ours", "reference")) {
            throw "unknown refusal side '$side' in ${Path}: $line"
        }
        $key = Census-Key $Profile $source
        if (-not $Rows.ContainsKey($key)) {
            $Rows[$key] = [ordered]@{
                name = $name
                profile = $Profile
                source = $source
                ours_refused = $false
                reference_refused = $false
                rc_ours = ""
                rc_reference = ""
            }
        }
        if ($Rows[$key].name -ne $name) {
            throw "census key collision for ${key}: '$($Rows[$key].name)' and '$name'"
        }
        if ($side -eq "ours") {
            $Rows[$key].ours_refused = $true
            $Rows[$key].rc_ours = $rc
        } else {
            $Rows[$key].reference_refused = $true
            $Rows[$key].rc_reference = $rc
        }
    }
}

function Add-ExcludeRows([hashtable]$Rows, [string]$Path) {
    if (-not $Path) { return }
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "exclude list missing: $Path"
    }
    foreach ($line in Get-Content -LiteralPath $Path) {
        $trim = $line.Trim()
        if (-not $trim -or $trim.StartsWith("#")) { continue }
        $parts = @($trim -split "\|", 4)
        if ($parts.Count -ne 4) {
            throw "malformed exclude row in ${Path}: $line"
        }
        $source = $parts[0]
        $activePath = $parts[2]
        $why = $parts[3]
        if ($activePath -notin @("general", "both")) { continue }
        $profile = "sce_fp_rsx"
        $key = Census-Key $profile $source
        if (-not $Rows.ContainsKey($key)) {
            $Rows[$key] = [ordered]@{
                name = [System.IO.Path]::GetFileNameWithoutExtension($source)
                profile = $profile
                source = $source
                ours_refused = $false
                reference_refused = $false
                rc_ours = ""
                rc_reference = ""
                ours_status_override = "excluded-poison"
                note = $why
            }
        }
    }
}

$bucketMap = Read-BucketMap $BucketMapCsv
$baseAccepted = Read-BaseAccepted $BaseCensusCsv
$rowsByKey = @{}

Add-RefusalRows $rowsByKey (Join-Path $SidecarRoot "reference-corpus-refused.txt") "sce_fp_rsx"
Add-RefusalRows $rowsByKey (Join-Path $SidecarRoot "reference-tree-corpus-refused.txt") "sce_fp_rsx"
Add-RefusalRows $rowsByKey (Join-Path $SidecarRoot "vp-corpus-refused.txt") "sce_vp_rsx"
Add-ExcludeRows $rowsByKey $ExcludeList

$outRows = foreach ($key in ($rowsByKey.Keys | Sort-Object)) {
    $raw = $rowsByKey[$key]
    $bucket = "unclassified"
    if ($bucketMap.BySource.ContainsKey($raw.source)) {
        $bucket = $bucketMap.BySource[$raw.source]
    } elseif ($bucketMap.ByName.ContainsKey($raw.name)) {
        $bucket = $bucketMap.ByName[$raw.name]
    }

    $oursStatus = if ($raw.ours_status_override) { $raw.ours_status_override } elseif ($raw.ours_refused) { "backend-refuse" } else { "accept" }
    $referenceStatus = if ($raw.reference_refused) { "backend-refuse" } else { "accept" }
    if ($raw.ours_refused -and $referenceStatus -eq "accept" -and $baseAccepted.ContainsKey($key)) {
        $oursStatus = "new-refuse"
    }

    [pscustomobject]@{
        name = $raw.name
        profile = $raw.profile
        source = $raw.source
        ours_status = $oursStatus
        reference_status = $referenceStatus
        bucket = $bucket
        rc_ours = $raw.rc_ours
        rc_reference = $raw.rc_reference
    }
}

$parent = Split-Path -Parent $OutputCsv
if ($parent -and -not (Test-Path -LiteralPath $parent -PathType Container)) {
    New-Item -ItemType Directory -Force $parent | Out-Null
}
$outRows = @($outRows)
if ($outRows.Count -eq 0) {
    Set-Content -LiteralPath $OutputCsv -Value '"name","profile","source","ours_status","reference_status","bucket","rc_ours","rc_reference"' -Encoding Ascii
} else {
    $outRows | Export-Csv -NoTypeInformation -Path $OutputCsv -Encoding Ascii
}
Write-Host ("CURRENT_CENSUS|rows={0}|sidecar_root={1}|output={2}" -f @($outRows).Count, $SidecarRoot, $OutputCsv)
