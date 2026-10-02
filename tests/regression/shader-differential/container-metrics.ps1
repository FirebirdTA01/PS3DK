$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot 'container-metrics-identity.ps1')
$script:containerCompileInputs = @{}
$script:containerMetricAttempts = @()

function Register-ContainerMetricAttempt([string]$Name, [string]$Role, [string]$Profile,
        [string]$Source, [string]$UniformSet, [string]$SourcePath) {
    $inputs=Get-ShaderCompileInputs $SourcePath @()
    # This describes an attempt, not a measured container. Successful rows
    # replace it at the gate; refused pairs remain visible without fake counts.
    $script:containerMetricAttempts += [pscustomobject]@{
        name=$Name;role=$Role;profile=$Profile;source=$Source;uniform_set=$UniformSet
        input_status='unversioned'
        input_reason=$(if($inputs.Reason) {"no successful pair: $($inputs.Reason)"} else {'no successful container pair'})
    }
}

function Read-BeU16([byte[]]$bytes, [int]$offset) {
    if ($offset -lt 0 -or $offset + 2 -gt $bytes.Length) {
        throw "container too short for u16 at 0x$($offset.ToString('x'))"
    }
    return ([uint16]$bytes[$offset] -shl 8) -bor [uint16]$bytes[$offset + 1]
}

function Read-BeU32([byte[]]$bytes, [int]$offset) {
    if ($offset -lt 0 -or $offset + 4 -gt $bytes.Length) {
        throw "container too short for u32 at 0x$($offset.ToString('x'))"
    }
    return ([uint32]$bytes[$offset] -shl 24) -bor
           ([uint32]$bytes[$offset + 1] -shl 16) -bor
           ([uint32]$bytes[$offset + 2] -shl 8) -bor
           [uint32]$bytes[$offset + 3]
}

function Read-ShaderContainerMetrics([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "container not found: $Path"
    }
    [byte[]]$bytes = [System.IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 32) {
        throw "container too short for CgBinaryProgram header: $Path"
    }

    $profile = Read-BeU32 $bytes 0
    $programOffset = [int](Read-BeU32 $bytes 20)
    if ($programOffset -le 0 -or $programOffset -ge $bytes.Length) {
        throw "container has invalid program offset 0x$($programOffset.ToString('x')): $Path"
    }

    switch ($profile) {
        0x00001b5c {
            if ($programOffset + 22 -gt $bytes.Length) {
                throw "fragment container too short for CgBinaryFragmentProgram: $Path"
            }
            return [pscustomobject]@{
                profile = "sce_fp_rsx"
                instruction_count = [int](Read-BeU32 $bytes $programOffset)
                register_count = [int]$bytes[$programOffset + 18]
            }
        }
        0x00001b5b {
            if ($programOffset + 24 -gt $bytes.Length) {
                throw "vertex container too short for CgBinaryVertexProgram: $Path"
            }
            return [pscustomobject]@{
                profile = "sce_vp_rsx"
                instruction_count = [int](Read-BeU32 $bytes $programOffset)
                register_count = [int](Read-BeU32 $bytes ($programOffset + 8))
            }
        }
        default {
            throw "unsupported Cg profile 0x$($profile.ToString('x8')) in container: $Path"
        }
    }
}

function Add-ContainerMetricsRow(
    [ref]$Rows,
    [string]$Name,
    [string]$Role,
    [string]$Profile,
    [string]$Source,
    [string]$UniformSet,
    [string]$OursPath,
    [string]$ReferencePath,
    [switch]$ByteIdentical,
    [switch]$Staged,
    [string]$PixelStatus = "unknown",
    [switch]$RequireInputIdentity
) {
    $ours = Read-ShaderContainerMetrics $OursPath
    $ref = Read-ShaderContainerMetrics $ReferencePath
    if ($ours.profile -ne $ref.profile) {
        throw "metrics profile mismatch for ${Name}: ours=$($ours.profile), reference=$($ref.profile)"
    }
    if ($Profile -and $ours.profile -ne $Profile) {
        throw "metrics profile mismatch for ${Name}: row=$Profile, container=$($ours.profile)"
    }

    $row = [pscustomobject]@{
        name = $Name
        role = $Role
        profile = $ours.profile
        source = $Source
        uniform_set = $UniformSet
        ours_instruction_count = $ours.instruction_count
        reference_instruction_count = $ref.instruction_count
        instruction_delta = $ours.instruction_count - $ref.instruction_count
        ours_register_count = $ours.register_count
        reference_register_count = $ref.register_count
        register_delta = $ours.register_count - $ref.register_count
        byte_identical = [bool]$ByteIdentical
        staged = [bool]$Staged
        pixel_status = $PixelStatus
    }
    if ($RequireInputIdentity) {
        $inputs=Join-ShaderCompileInputs `
            $script:containerCompileInputs[[IO.Path]::GetFullPath($OursPath)] `
            $script:containerCompileInputs[[IO.Path]::GetFullPath($ReferencePath)]
        foreach($property in $inputs.PSObject.Properties) { $row | Add-Member NoteProperty $property.Name $property.Value }
    }
    $Rows.Value += $row
}

function Get-ContainerMetricsSummary([object[]]$Rows) {
    function Metric-Int($value) {
        if ($null -eq $value -or $value -eq "") { return 0 }
        return [int]$value
    }
    function Metric-Bool($value) {
        if ($value -is [bool]) { return $value }
        return "$value".ToLowerInvariant() -eq "true"
    }

    $compared = @($Rows).Count
    $instructionMismatches = @($Rows | Where-Object { (Metric-Int $_.instruction_delta) -ne 0 }).Count
    $registerMismatches = @($Rows | Where-Object { (Metric-Int $_.register_delta) -ne 0 }).Count
    $bothMismatches = @($Rows | Where-Object {
        (Metric-Int $_.instruction_delta) -ne 0 -and
        (Metric-Int $_.register_delta) -ne 0
    }).Count
    $worseInstructions = @($Rows | Where-Object { (Metric-Int $_.instruction_delta) -gt 0 }).Count
    $betterInstructions = @($Rows | Where-Object { (Metric-Int $_.instruction_delta) -lt 0 }).Count
    $worseRegisters = @($Rows | Where-Object { (Metric-Int $_.register_delta) -gt 0 }).Count
    $betterRegisters = @($Rows | Where-Object { (Metric-Int $_.register_delta) -lt 0 }).Count
    $pixelProofRows = @($Rows | Where-Object {
        $_.pixel_status -eq "identical" -and
        (Metric-Int $_.register_delta) -ne 0
    }).Count
    $pixelProofCandidates = @($Rows | Where-Object {
        -not (Metric-Bool $_.byte_identical) -and
        (Metric-Int $_.register_delta) -ne 0
    }).Count

    return [pscustomobject]@{
        Compared = $compared
        InstructionMismatches = $instructionMismatches
        RegisterMismatches = $registerMismatches
        BothMismatches = $bothMismatches
        WorseInstructions = $worseInstructions
        BetterInstructions = $betterInstructions
        WorseRegisters = $worseRegisters
        BetterRegisters = $betterRegisters
        PixelProofRows = $pixelProofRows
        PixelProofCandidates = $pixelProofCandidates
    }
}

function Get-ContainerMetricKey($Row) {
    return "$($Row.role)|$($Row.name)|$($Row.profile)|$($Row.source)|$($Row.uniform_set)"
}

function Get-ContainerMetricsGateSummary([object[]]$Rows, [object[]]$BaselineRows, [object[]]$AttemptedInputs = @()) {
    function Metric-Int($value) {
        if ($null -eq $value -or $value -eq "") { return 0 }
        return [int]$value
    }

    $index = Get-ContainerMetricBaselineIndex $BaselineRows
    $baselineByKey = $index.Rows
    $identityMismatches=0; $missingIdentities=0; $sourceMismatches=0
    $flagMismatches=0; $includeMismatches=0; $oracleMismatches=0
    $currentKeys=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach($r in $Rows) { $null=$currentKeys.Add((Get-ContainerMetricKey $r)) }
    $missingInputs=@($AttemptedInputs | Where-Object {-not $currentKeys.Contains((Get-ContainerMetricKey $_))})
    $missingVersionedResults=@($missingInputs | Where-Object {$index.Versions.ContainsKey((Get-ContainerMetricKey $_))} | ForEach-Object {Get-ContainerMetricKey $_})
    $identityMismatches += $missingVersionedResults.Count

    $baselineRegressions = 0
    $missingBaselineRows = 0
    $worstInstructionRegression = 0
    $worstRegisterRegression = 0
    $currentRows = @($Rows).Count
    foreach ($row in @($Rows)) {
        $key = Get-ContainerMetricKey $row
        $identity = Get-ContainerMetricIdentity $row
        if ($index.Versions.ContainsKey($key)) {
            $versions=$index.Versions[$key]
            $versionKey="$key|v1:$identity"
            if (-not $identity -or -not $baselineByKey.ContainsKey($versionKey)) {
                $identityMismatches++; $missingBaselineRows++
                if (-not $identity) { $missingIdentities++ }
                else {
                    # More than one axis may have changed. Compare each axis
                    # against the recorded versions; do not mislabel tool drift.
                    if ($row.source_sha256 -cnotin @($versions.source_sha256)) { $sourceMismatches++ }
                    if ($row.compile_flags -cnotin @($versions.compile_flags)) { $flagMismatches++ }
                    if ($row.resolved_includes -cnotin @($versions.resolved_includes)) { $includeMismatches++ }
                    if ($row.oracle_sha256 -cnotin @($versions.oracle_sha256)) { $oracleMismatches++ }
                }
                continue
            }
            $key=$versionKey
        }
        if (-not $baselineByKey.ContainsKey($key)) {
            $missingBaselineRows++
            continue
        }
        $baseline = $baselineByKey[$key]
        $instructionRegression = (Metric-Int $row.instruction_delta) - (Metric-Int $baseline.instruction_delta)
        $registerRegression = (Metric-Int $row.register_delta) - (Metric-Int $baseline.register_delta)
        if ($instructionRegression -gt 0 -or $registerRegression -gt 0) {
            $baselineRegressions++
            if ($instructionRegression -gt $worstInstructionRegression) {
                $worstInstructionRegression = $instructionRegression
            }
            if ($registerRegression -gt $worstRegisterRegression) {
                $worstRegisterRegression = $registerRegression
            }
        }
    }

    $baselineRowCount = @($BaselineRows).Count
    $comparedBaselineRows = $currentRows - $missingBaselineRows
    $status = if ($identityMismatches -gt 0) {
        "identity-mismatch"
    } elseif ($baselineRegressions -gt 0) {
        "fail"
    } elseif ($baselineRowCount -gt 0 -and $currentRows -gt 0 -and $comparedBaselineRows -eq 0) {
        "no-coverage"
    } else {
        "ok"
    }

    return [pscustomobject]@{
        Status = $status
        CurrentRows = $currentRows
        BaselineRows = $baselineRowCount
        ComparedBaselineRows = $comparedBaselineRows
        MissingBaselineRows = $missingBaselineRows
        BaselineRegressions = $baselineRegressions
        WorstInstructionRegression = $worstInstructionRegression
        WorstRegisterRegression = $worstRegisterRegression
        VersionedBaselineIdentities = $index.Versions.Count
        IdentityMismatches = $identityMismatches
        MissingIdentityRows = $missingIdentities
        SourceVersionMismatches = $sourceMismatches
        CompileFlagsMismatches = $flagMismatches
        IncludeMismatches = $includeMismatches
        OracleMismatches = $oracleMismatches
        MissingVersionedResults = $missingVersionedResults
        UnversionedRows = @((@($Rows)+@($missingInputs)) | Where-Object { $_.input_status -eq 'unversioned' } | ForEach-Object {
            [pscustomobject]@{Key=(Get-ContainerMetricKey $_);Reason=$_.input_reason}
        })
    }
}

function Write-ContainerMetricsGateReport([object[]]$Rows, [string]$BaselinePath, [switch]$ReportOnly, [string]$AllowancePath = '', [object[]]$AttemptedInputs = @()) {
    $baselinePresent = Test-Path -LiteralPath $BaselinePath -PathType Leaf
    $baselineRows = @()
    if ($baselinePresent) {
        $header = Get-Content -LiteralPath $BaselinePath -TotalCount 1
        if (-not $header) {
            throw "baseline CSV has no header: $BaselinePath"
        }
        $columns = @($header -split "," | ForEach-Object { $_.Trim().Trim('"') })
        foreach ($column in @("role", "name", "profile", "source", "uniform_set", "instruction_delta", "register_delta")) {
            if ($column -notin $columns) {
                throw "baseline CSV missing required column '$column': $BaselinePath"
            }
        }
        $baselineRows = @(Import-Csv -LiteralPath $BaselinePath)
    }

    $allowanceMatch = $null
    if ($AllowancePath) {
        if (-not $baselinePresent) { throw 'container metrics allowance requires an existing baseline CSV' }
        $allowanceMatch = Get-ContainerMetricsAllowanceMatch $Rows $baselineRows $AllowancePath
    }
    $summary = Get-ContainerMetricsGateSummary $Rows $baselineRows -AttemptedInputs $AttemptedInputs
    $mode = if ($baselinePresent -and -not $ReportOnly) { "fail-by-default" } else { "report-only" }
    $effectiveStatus = $summary.Status
    if ($allowanceMatch) {
        $allowed = $allowanceMatch.MatchedKeys.Count
        $unallowed = $summary.BaselineRegressions - $allowed
        if ($unallowed -lt 0) { throw 'container metrics allowance exceeds raw regression count' }
        if ($summary.Status -eq 'fail' -and $allowed -gt 0 -and $unallowed -eq 0) { $effectiveStatus = 'allowed' }
    }
    $shouldFail = $mode -eq "fail-by-default" -and $effectiveStatus -notin @('ok', 'allowed')
    $baselineState = if ($baselinePresent) { "present" } else { "absent" }
    Write-Host "SDIFF-METRICS-GATE|status=$($summary.Status)|mode=$mode|current_rows=$($summary.CurrentRows)|baseline_rows=$($summary.BaselineRows)|compared_baseline_rows=$($summary.ComparedBaselineRows)|missing_baseline_rows=$($summary.MissingBaselineRows)|baseline_regressions=$($summary.BaselineRegressions)|worst_instruction_regression=$($summary.WorstInstructionRegression)|worst_register_regression=$($summary.WorstRegisterRegression)|baseline=$baselineState"
    if($summary.VersionedBaselineIdentities -gt 0) {
        Write-Host "SDIFF-METRICS-IDENTITY|mismatches=$($summary.IdentityMismatches)|missing=$($summary.MissingIdentityRows)|source=$($summary.SourceVersionMismatches)|flags=$($summary.CompileFlagsMismatches)|includes=$($summary.IncludeMismatches)|oracle=$($summary.OracleMismatches)|missing_results=$($summary.MissingVersionedResults.Count)"
        foreach($key in $summary.MissingVersionedResults) { Write-Host "SDIFF-METRICS-MISSING-VERSIONED-RESULT|key=$key" }
    }
    if(@($Rows | Where-Object { $_.input_status }).Count -gt 0 -or $AttemptedInputs.Count -gt 0) {
        Write-Host "SDIFF-METRICS-UNVERSIONED|count=$($summary.UnversionedRows.Count)"
        foreach($unversioned in $summary.UnversionedRows) {
            Write-Host "SDIFF-METRICS-UNVERSIONED-ROW|key=$($unversioned.Key)|reason=$($unversioned.Reason)"
        }
    }
    $result = [pscustomobject]@{
        Summary = $summary
        Mode = $mode
        ShouldFail = $shouldFail
        BaselinePath = $BaselinePath
        BaselinePresent = $baselinePresent
    }
    if ($allowanceMatch) {
        $result | Add-Member NoteProperty EffectiveStatus $effectiveStatus
        $result | Add-Member NoteProperty AllowedRegressions $allowed
        $result | Add-Member NoteProperty UnallowedRegressions $unallowed
        $result | Add-Member NoteProperty AllowedKeys $allowanceMatch.MatchedKeys
        $result | Add-Member NoteProperty Allowance $allowanceMatch.Pin
        Write-Host "SDIFF-METRICS-ALLOWANCE|effective_status=$effectiveStatus|matched=$allowed|unmatched=$unallowed|path=$($allowanceMatch.Pin.Path)|sha256=$($allowanceMatch.Pin.Sha256)"
    }
    return $result
}

. (Join-Path $PSScriptRoot 'container-metrics-allowances.ps1')

# This is also the stager's boundary: persist evidence before enforcing failure.
function Invoke-ContainerMetricsStageGate([object[]]$Rows, [string]$BaselinePath,
        [switch]$ReportOnly, [string]$AllowancePath = '', [string]$EvidencePath = '', [object[]]$AttemptedInputs = @()) {
    $gate = Write-ContainerMetricsGateReport $Rows $BaselinePath -ReportOnly:$ReportOnly -AllowancePath $AllowancePath -AttemptedInputs $AttemptedInputs
    if ($AllowancePath) {
        if (-not $EvidencePath) { throw 'container metrics allowance requires a gate evidence path' }
        $gate | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $EvidencePath -Encoding UTF8
    }
    if ($gate.ShouldFail) {
        if($gate.Summary.IdentityMismatches -gt 0) { throw "container metrics identity gate failed: $($gate.Summary.IdentityMismatches) unmatched versioned input(s)" }
        throw "container metrics gate failed: $($gate.Summary.BaselineRegressions) baseline regression(s)"
    }
    return $gate
}

function Write-ContainerMetricsReport([object[]]$Rows, [string]$Path) {
    $Rows | Export-Csv -NoTypeInformation -Path $Path -Encoding UTF8
    $s = Get-ContainerMetricsSummary $Rows
    Write-Host "SDIFF-METRICS|compared=$($s.Compared)|instruction_mismatches=$($s.InstructionMismatches)|register_mismatches=$($s.RegisterMismatches)|both_mismatches=$($s.BothMismatches)|worse_instructions=$($s.WorseInstructions)|better_instructions=$($s.BetterInstructions)|worse_registers=$($s.WorseRegisters)|better_registers=$($s.BetterRegisters)|pixel_proof_candidates=$($s.PixelProofCandidates)|pixel_proof_rows=$($s.PixelProofRows)"
    Write-Host "container metrics written to $Path"
}

function Parse-SdiffRows([string]$Text) {
    $rows = @()
    foreach ($line in ($Text -split "`r?`n")) {
        if (-not $line.StartsWith("SDIFF|")) { continue }
        $row = @{}
        foreach ($part in ($line.Split("|") | Select-Object -Skip 1)) {
            $eq = $part.IndexOf("=")
            if ($eq -lt 1) { continue }
            $row[$part.Substring(0, $eq)] = $part.Substring($eq + 1)
        }
        if ($row.ContainsKey("shader") -and $row.ContainsKey("status")) {
            $rows += [pscustomobject]$row
        }
    }
    return $rows
}

function Join-ContainerMetricsWithSdiff([object[]]$Rows, [object[]]$SdiffRows) {
    $statusByShader = @{}
    foreach ($row in @($SdiffRows)) {
        if ($row.shader -and $row.status) {
            $statusByShader[$row.shader] = $row.status
        }
    }

    foreach ($row in @($Rows)) {
        if ($statusByShader.ContainsKey($row.name)) {
            $row.pixel_status = $statusByShader[$row.name]
        }
    }
    return $Rows
}
