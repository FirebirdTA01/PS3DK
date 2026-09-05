$ErrorActionPreference = "Stop"

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
    [string]$PixelStatus = "unknown"
) {
    $ours = Read-ShaderContainerMetrics $OursPath
    $ref = Read-ShaderContainerMetrics $ReferencePath
    if ($ours.profile -ne $ref.profile) {
        throw "metrics profile mismatch for ${Name}: ours=$($ours.profile), reference=$($ref.profile)"
    }
    if ($Profile -and $ours.profile -ne $Profile) {
        throw "metrics profile mismatch for ${Name}: row=$Profile, container=$($ours.profile)"
    }

    $Rows.Value += [pscustomobject]@{
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

function Get-ContainerMetricsGateSummary([object[]]$Rows, [object[]]$BaselineRows) {
    function Metric-Int($value) {
        if ($null -eq $value -or $value -eq "") { return 0 }
        return [int]$value
    }

    $baselineByKey = @{}
    foreach ($row in @($BaselineRows)) {
        $key = Get-ContainerMetricKey $row
        if ($baselineByKey.ContainsKey($key)) {
            throw "duplicate container metrics baseline row: $key"
        }
        $baselineByKey[$key] = $row
    }

    $baselineRegressions = 0
    $missingBaselineRows = 0
    $worstInstructionRegression = 0
    $worstRegisterRegression = 0
    $currentRows = @($Rows).Count
    foreach ($row in @($Rows)) {
        $key = Get-ContainerMetricKey $row
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
    $status = if ($baselineRegressions -gt 0) {
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
    }
}

function Write-ContainerMetricsGateReport([object[]]$Rows, [string]$BaselinePath, [switch]$ReportOnly) {
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

    $summary = Get-ContainerMetricsGateSummary $Rows $baselineRows
    $mode = if ($baselinePresent -and -not $ReportOnly) { "fail-by-default" } else { "report-only" }
    $shouldFail = $mode -eq "fail-by-default" -and $summary.Status -ne "ok"
    $baselineState = if ($baselinePresent) { "present" } else { "absent" }
    Write-Host "SDIFF-METRICS-GATE|status=$($summary.Status)|mode=$mode|current_rows=$($summary.CurrentRows)|baseline_rows=$($summary.BaselineRows)|compared_baseline_rows=$($summary.ComparedBaselineRows)|missing_baseline_rows=$($summary.MissingBaselineRows)|baseline_regressions=$($summary.BaselineRegressions)|worst_instruction_regression=$($summary.WorstInstructionRegression)|worst_register_regression=$($summary.WorstRegisterRegression)|baseline=$baselineState"
    return [pscustomobject]@{
        Summary = $summary
        Mode = $mode
        ShouldFail = $shouldFail
        BaselinePath = $BaselinePath
        BaselinePresent = $baselinePresent
    }
}

function Write-ContainerMetricsReport([object[]]$Rows, [string]$Path) {
    $Rows | Export-Csv -NoTypeInformation -Path $Path -Encoding Ascii
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
