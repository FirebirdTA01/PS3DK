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
