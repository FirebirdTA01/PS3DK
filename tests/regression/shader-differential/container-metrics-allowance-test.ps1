# Isolated controls: no compilers, build slots, or emulator.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'container-metrics.ps1')
$work = Join-Path $env:TEMP ('sd-metrics-allowance-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
$script:checks = 0
function Check($ok, [string]$label) {
    if (-not $ok) { throw "FAIL: $label" }
    $script:checks++
    Write-Host "PASS: $label"
}
function Clone($value) { return ($value | ConvertTo-Json -Depth 20 | ConvertFrom-Json) }
function Counts([int]$ours = 13, [int]$reference = 2) {
    return [pscustomobject]@{
        ours_instruction_count = $ours; reference_instruction_count = $reference
        instruction_delta = $ours - $reference
        ours_register_count = 2; reference_register_count = 1; register_delta = 1
    }
}
function Row([string]$name, [int]$ours) {
    $r = Counts $ours
    foreach ($p in @{role='vp-reference'; name=$name; profile='sce_vp_rsx'; source="shaders/$name.cg"; uniform_set='auto'}.GetEnumerator()) {
        $r | Add-Member NoteProperty $p.Key $p.Value
    }
    return $r
}
function Entry($row) {
    return [pscustomobject]@{
        role=$row.role; name=$row.name; profile=$row.profile; source=$row.source; uniform_set=$row.uniform_set
        card='descriptive-card'; reason='Pinned historical cost growth'; count=1
        historical=(Counts 11); current=(Counts 13)
    }
}
function Save($doc) { $doc | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $allowance -Encoding UTF8 }
function Reject([string]$label, [scriptblock]$action, [string]$message = 'allowance') {
    $errorText = ''
    try { & $action | Out-Null } catch { $errorText = $_.Exception.Message }
    Check ($errorText -like "*$message*") "$label ($errorText)"
}
try {
    $baseline = Join-Path $work 'baseline.csv'
    $allowance = Join-Path $work 'allowance.json'
    $rows = @((Row 'first' 13), (Row 'second' 13))
    @((Row 'first' 11), (Row 'second' 11)) | Export-Csv -NoTypeInformation -LiteralPath $baseline
    $doc = [pscustomobject]@{schema_version=1; entries=@((Entry $rows[0]), (Entry $rows[1]))}
    Save $doc
    $plain = Write-ContainerMetricsGateReport $rows $baseline 6>$null
    $empty = Write-ContainerMetricsGateReport $rows $baseline -AllowancePath '' 6>$null
    Check (($plain | ConvertTo-Json -Depth 8 -Compress) -ceq ($empty | ConvertTo-Json -Depth 8 -Compress)) 'empty option preserves legacy result'
    Check ($plain.ShouldFail -and $plain.Summary.BaselineRegressions -eq 2) 'no allowance keeps the historical gate red'
    $green = Write-ContainerMetricsGateReport $rows $baseline -AllowancePath $allowance 6>$null
    Check (-not $green.ShouldFail -and $green.EffectiveStatus -ceq 'allowed' -and $green.Summary.Status -ceq 'fail' -and $green.AllowedRegressions -eq 2 -and $green.UnallowedRegressions -eq 0) 'all exact entries allow the stage while retaining raw failures'
    Check ($green.Allowance.Path -ceq (Get-Item -LiteralPath $allowance).FullName -and $green.Allowance.Sha256 -ceq (Get-FileHash -LiteralPath $allowance -Algorithm SHA256).Hash.ToLowerInvariant()) 'gate records the exact allowance file hash'
    Assert-ContainerMetricsAllowancePin $green.Allowance
    Check $true 'unchanged allowance pin verifies'
    $evidence = Join-Path $work 'container-metrics-gate.json'
    $stageGreen = Invoke-ContainerMetricsStageGate $rows $baseline -AllowancePath $allowance -EvidencePath $evidence 6>$null
    $saved = Get-Content -Raw -LiteralPath $evidence | ConvertFrom-Json
    Check ($stageGreen.EffectiveStatus -ceq 'allowed' -and $saved.Allowance.Sha256 -ceq $green.Allowance.Sha256) 'stager green persists the applied allowance pin'

    $mixed = Clone $doc; $mixed.entries = @($mixed.entries[0]); Save $mixed
    $red = Write-ContainerMetricsGateReport $rows $baseline -AllowancePath $allowance 6>$null
    Check ($red.ShouldFail -and $red.EffectiveStatus -ceq 'fail' -and $red.AllowedRegressions -eq 1 -and $red.UnallowedRegressions -eq 1) 'matched plus unmatched regression fails the stage'
    Reject 'stager red with a matched and an unmatched row' { Invoke-ContainerMetricsStageGate $rows $baseline -AllowancePath $allowance -EvidencePath $evidence 6>$null } 'container metrics gate failed:'
    $saved = Get-Content -Raw -LiteralPath $evidence | ConvertFrom-Json
    Check ($saved.ShouldFail -and $saved.UnallowedRegressions -eq 1 -and $saved.AllowedRegressions -eq 1) 'failed stage preserves mixed result evidence'
    Reject 'hand edit between staging and boot' { Assert-ContainerMetricsAllowancePin $green.Allowance }
    $blank = Clone $doc; $blank.entries=@(); Save $blank
    $red = Write-ContainerMetricsGateReport $rows $baseline -AllowancePath $allowance 6>$null
    Check ($red.ShouldFail -and $red.UnallowedRegressions -eq 2) 'empty entries waive nothing'

    $mutations = [ordered]@{
        'missing card' = {param($d) $d.entries[0].PSObject.Properties.Remove('card')}
        'blank reason' = {param($d) $d.entries[0].reason=' '}
        'numeric card' = {param($d) $d.entries[0].card=1}
        'wrong version' = {param($d) $d.schema_version=2}
        'string version' = {param($d) $d.schema_version='1'}
        'unknown property' = {param($d) $d | Add-Member NoteProperty entry @()}
        'string count' = {param($d) $d.entries[0].count='1'}
        'zero count' = {param($d) $d.entries[0].count=0}
        'duplicate allowance' = {param($d) $d.entries=@($d.entries[0],$d.entries[0])}
        'missing metrics' = {param($d) $d.entries[0].current.PSObject.Properties.Remove('ours_register_count')}
        'negative raw count' = {param($d) $d.entries[0].current.ours_register_count=-1}
        'inconsistent delta' = {param($d) $d.entries[0].current.instruction_delta=12}
        'fractional metric' = {param($d) $d.entries[0].current.ours_instruction_count=13.5}
        'null metric' = {param($d) $d.entries[0].current.ours_instruction_count=$null}
        'string metric' = {param($d) $d.entries[0].current.ours_instruction_count='13'}
        'case changed key' = {param($d) $d.entries[0].name='FIRST'}
        'renamed source' = {param($d) $d.entries[0].source='shaders/other.cg'}
        'historical drift' = {param($d) $d.entries[0].historical=Counts 10}
        'stale current counts' = {param($d) $d.entries[0].current=Counts 14}
    }
    foreach ($label in $mutations.Keys) {
        $bad = Clone $doc; & $mutations[$label] $bad; Save $bad
        Reject $label { Write-ContainerMetricsGateReport $rows $baseline -AllowancePath $allowance 6>$null }
    }
    Save $doc
    $raw = $doc | ConvertTo-Json -Depth 20
    foreach ($duplicate in @(
        @('schema_version', '"schema_version":99,"schema_version":'),
        @('card', '"card":"old","card":'),
        @('ours_instruction_count', '"ours_instruction_count":999,"ours_instruction_count":'),
        @('reason', '"\u0072eason":"old","reason":')
    )) {
        $raw.Replace(('"' + $duplicate[0] + '":'), $duplicate[1]) | Set-Content -LiteralPath $allowance -Encoding UTF8
        Reject "duplicate JSON member $($duplicate[0]) with ReportOnly" { Write-ContainerMetricsGateReport $rows $baseline -AllowancePath $allowance -ReportOnly 6>$null }
    }
    Save $doc
    foreach ($invalid in @(
        '{schema_version:99,schema_version:1,entries:[]}',
        "{'schema_version':99,'schema_version':1,'entries':[]}",
        '{schema_version:99,"schema_version":1,"entries":[]}',
        '{"schema_version":1,"entries":[],}',
        '{"schema_version":1,"entries":[,]}',
        '{"schema_version":1,"entries":[]} "'
    )) {
        Set-Content -LiteralPath $allowance -Value $invalid -Encoding UTF8
        Reject "non-JSON syntax with ReportOnly: $invalid" { Write-ContainerMetricsGateReport $rows $baseline -AllowancePath $allowance -ReportOnly 6>$null }
    }
    Save $doc
    Reject 'missing current row' { Write-ContainerMetricsGateReport @($rows[0]) $baseline -AllowancePath $allowance 6>$null }
    Reject 'duplicate current row' { Write-ContainerMetricsGateReport @($rows[0],$rows[0],$rows[1]) $baseline -AllowancePath $allowance 6>$null }
    Reject 'fixed row forces allowance removal' { Write-ContainerMetricsGateReport @((Row 'first' 11),$rows[1]) $baseline -AllowancePath $allowance 6>$null }
    Reject 'growth in instruction count' { Write-ContainerMetricsGateReport @((Row 'first' 14),$rows[1]) $baseline -AllowancePath $allowance 6>$null }
    $sameDelta = Row 'first' 14; $sameDelta.reference_instruction_count=3; $sameDelta.instruction_delta=11
    Reject 'compensating reference growth still rejected' { Write-ContainerMetricsGateReport @($sameDelta,$rows[1]) $baseline -AllowancePath $allowance 6>$null }
    Reject 'allowance requires baseline' { Write-ContainerMetricsGateReport $rows (Join-Path $work 'absent.csv') -AllowancePath $allowance 6>$null }
    Set-Content -LiteralPath $allowance -Value '{broken' -Encoding UTF8
    Reject 'malformed JSON with ReportOnly' { Write-ContainerMetricsGateReport $rows $baseline -AllowancePath $allowance -ReportOnly 6>$null }
    Reject 'missing allowance file' { Write-ContainerMetricsGateReport $rows $baseline -AllowancePath (Join-Path $work 'missing.json') 6>$null }
    Write-Host "container-metrics-allowance-test: tests=$script:checks pass=$script:checks fail=0"
} finally {
    # Generated unique TEMP directory; never remove outside this resolved root.
    if ((Split-Path -Parent $work) -ne $env:TEMP) { throw "unsafe test cleanup: $work" }
    Remove-Item -LiteralPath $work -Recurse -Force
}
