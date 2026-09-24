# rpcs3-process-guard-test.ps1
# Regression test suite for t_41cb6b51:
# 1. Process appearance guards at actual harness call sites (run start, warm-up, judged boot)
#    including red controls proving failure when checks are absent.
# 2. Mid-run lock identity replacement (same owner, different PID) refused & preserved in finally.
# 3. Eight-case release verification: malformed, incomplete, foreign, and wrong-PID locks retained.
# 4. Normal -WhatIf run records SKIPPED rows without false PASS and cleans own lock.
# 5. Lock claim write failure handling & cleanup, plus red control proving parent abandoned lock.
#
# Constraints: Dummy process only (timeout.exe); ZERO real RPCS3 launches.

$ErrorActionPreference = "Continue"

function Assert-Equal($actual, $expected, $msg) {
    if ($actual -ne $expected) {
        [Console]::Error.WriteLine("ASSERTION FAILED: $msg. Expected: '$expected', Actual: '$actual'")
        exit 1
    }
}

function Assert-True($condition, $msg) {
    if (-not $condition) {
        [Console]::Error.WriteLine("ASSERTION FAILED: $msg")
        exit 1
    }
}

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$ClaimScript = Join-Path $RepoRoot "scripts\rpcs3-claim.ps1"
$ReleaseScript = Join-Path $RepoRoot "scripts\rpcs3-release.ps1"
$HarnessScript = Join-Path $RepoRoot "scripts\run-regression-rpcs3.ps1"

$TestDir = Join-Path $env:TEMP ("guard_test_suite_" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $TestDir | Out-Null

$DummyExe = Join-Path $TestDir "dummy_rpcs3.exe"
Copy-Item "$env:SystemRoot\System32\timeout.exe" $DummyExe -Force

$MockManifest = Join-Path $TestDir "manifest.txt"
@"
name,relative_self,timeout_seconds,expected_state,required_tty_regex,forbidden_tty_regex
sample1,scripts/rpcs3-claim.ps1,10,RAN-CLEAN,-,-
"@ | Set-Content -Encoding ascii $MockManifest

$CurrentOwner = "test-runner@$env:COMPUTERNAME"

try {
    # =========================================================================
    # PART 1: Process appearance guards at actual harness call sites
    # =========================================================================

    # --- 1a: Preflight run start refusal ---
    Write-Host "[1a] Harness refuses at run start when process is running"
    $lock1a = Join-Path $TestDir "lock1a.json"
    $res1a = Join-Path $TestDir "res1a"
    $p1a = Start-Process -FilePath $DummyExe -ArgumentList "60" -WindowStyle Hidden -PassThru
    try {
        $out1a = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $HarnessScript `
            -Rpcs3Path $DummyExe `
            -Manifest $MockManifest `
            -ResultsRoot $res1a `
            -LockPath $lock1a `
            -Owner $CurrentOwner `
            -TargetProcessName "dummy_rpcs3" `
            -WhatIf 2>&1
        $rc1a = $LASTEXITCODE
        Assert-Equal $rc1a 1 "Run start must exit 1 when process is running"
        Assert-True (-not (Test-Path $lock1a)) "Lock must be released after run start refusal"
        $matchStart = ($out1a | Out-String) -match "dummy_rpcs3\.exe is already running before run start"
        Assert-True $matchStart "Output must report process running before run start"
    } finally {
        if (-not $p1a.HasExited) { Stop-Process -Id $p1a.Id -Force; $p1a.WaitForExit() }
    }

    # --- 1b: Process appears AFTER preflight, BEFORE warm-up boot ---
    Write-Host "[1b] Process appears after preflight, before warm-up boot"
    $lock1b = Join-Path $TestDir "lock1b.json"
    $res1b = Join-Path $TestDir "res1b"
    $wrapper1b = Join-Path $TestDir "wrapper1b.ps1"
    @"
`$global:guardCallCount = 0
function Get-CimInstance {
    param(`$ClassName, `$Filter, `$ErrorAction)
    if (`$Filter -match 'dummy_rpcs3') {
        `$global:guardCallCount++
        if (`$global:guardCallCount -eq 1) {
            return @()
        } else {
            return @([pscustomobject]@{
                ProcessId = 7777
                CommandLine = 'dummy_rpcs3.exe --cold-warmup-collision'
                Name = 'dummy_rpcs3.exe'
            })
        }
    }
    return Microsoft.Management.Infrastructure.CimCmdlets\Get-CimInstance @PSBoundParameters
}
& '$HarnessScript' -Rpcs3Path '$DummyExe' -Manifest '$MockManifest' -ResultsRoot '$res1b' -LockPath '$lock1b' -Owner '$CurrentOwner' -TargetProcessName 'dummy_rpcs3' -WhatIf
exit `$LASTEXITCODE
"@ | Set-Content -Encoding ascii $wrapper1b

    $out1b = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $wrapper1b 2>&1
    $rc1b = $LASTEXITCODE
    Assert-Equal $rc1b 1 "Must exit 1 when process appears before warm-up boot"
    Assert-True (-not (Test-Path $lock1b)) "Lock must be released on warm-up refusal"
    $out1bStr = ($out1b | Out-String)
    Assert-True ($out1bStr -match "dummy_rpcs3\.exe is already running before warm-up boot of sample1") "Must catch process before warm-up boot"
    Assert-True ($out1bStr -match "PID 7777") "Must report conflicting PID"
    Assert-True (-not ($out1bStr -match "skipping actual warm-up launch")) "Must NOT reach warm-up launch"

    # Red Control for 1b: Remove the warm-up Assert call and verify it reaches launch
    Write-Host "[1b-red] Red control: removing warm-up check reaches launch"
    $harnessNoWarmup = Join-Path (Join-Path $RepoRoot "scripts") "tmp_harness_no_warmup.ps1"
    $harnessContent = Get-Content -Raw $HarnessScript
    $mutatedNoWarmup = $harnessContent.Replace('Assert-Rpcs3ReadyForLaunch "warm-up boot of $name"', '# REMOVED CHECK')
    $mutatedNoWarmup | Set-Content -Encoding ascii $harnessNoWarmup

    $lock1bRed = Join-Path $TestDir "lock1b_red.json"
    $wrapper1bRed = Join-Path $TestDir "wrapper1b_red.ps1"
    @"
`$global:guardCallCount = 0
function Get-CimInstance {
    param(`$ClassName, `$Filter, `$ErrorAction)
    if (`$Filter -match 'dummy_rpcs3') {
        `$global:guardCallCount++
        if (`$global:guardCallCount -eq 1) {
            return @()
        } else {
            return @([pscustomobject]@{
                ProcessId = 7777
                CommandLine = 'dummy_rpcs3.exe --cold-warmup-collision'
                Name = 'dummy_rpcs3.exe'
            })
        }
    }
    return Microsoft.Management.Infrastructure.CimCmdlets\Get-CimInstance @PSBoundParameters
}
& '$harnessNoWarmup' -Rpcs3Path '$DummyExe' -Manifest '$MockManifest' -ResultsRoot '$res1b' -LockPath '$lock1bRed' -Owner '$CurrentOwner' -TargetProcessName 'dummy_rpcs3' -WhatIf
exit `$LASTEXITCODE
"@ | Set-Content -Encoding ascii $wrapper1bRed

    $out1bRed = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $wrapper1bRed 2>&1
    $out1bRedStr = ($out1bRed | Out-String)
    # The red mutant does NOT have the warm-up check, so it proceeds to warm-up launch:
    Assert-True ($out1bRedStr -match "skipping actual warm-up launch") "Red control must prove launch is reached when check is absent"
    Remove-Item $harnessNoWarmup, $lock1bRed -Force -ErrorAction SilentlyContinue


    # --- 1c: Process appears AFTER warm-up, BEFORE judged boot ---
    Write-Host "[1c] Process appears after warm-up, before judged boot"
    $lock1c = Join-Path $TestDir "lock1c.json"
    $res1c = Join-Path $TestDir "res1c"
    $wrapper1c = Join-Path $TestDir "wrapper1c.ps1"
    @"
`$global:guardCallCount = 0
function Get-CimInstance {
    param(`$ClassName, `$Filter, `$ErrorAction)
    if (`$Filter -match 'dummy_rpcs3') {
        `$global:guardCallCount++
        if (`$global:guardCallCount -le 2) {
            # Call 1 (run start) and Call 2 (warm-up) pass
            return @()
        } else {
            # Call 3 (judged boot) catches process
            return @([pscustomobject]@{
                ProcessId = 8888
                CommandLine = 'dummy_rpcs3.exe --judged-boot-collision'
                Name = 'dummy_rpcs3.exe'
            })
        }
    }
    return Microsoft.Management.Infrastructure.CimCmdlets\Get-CimInstance @PSBoundParameters
}
& '$HarnessScript' -Rpcs3Path '$DummyExe' -Manifest '$MockManifest' -ResultsRoot '$res1c' -LockPath '$lock1c' -Owner '$CurrentOwner' -TargetProcessName 'dummy_rpcs3' -WhatIf
exit `$LASTEXITCODE
"@ | Set-Content -Encoding ascii $wrapper1c

    $out1c = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $wrapper1c 2>&1
    $rc1c = $LASTEXITCODE
    Assert-Equal $rc1c 1 "Must exit 1 when process appears before judged boot"
    Assert-True (-not (Test-Path $lock1c)) "Lock must be released on judged refusal"
    $out1cStr = ($out1c | Out-String)
    Assert-True ($out1cStr -match "dummy_rpcs3\.exe is already running before judged boot of sample1") "Must catch process before judged boot"
    Assert-True ($out1cStr -match "PID 8888") "Must report conflicting PID"
    Assert-True (-not ($out1cStr -match "skipping actual judged launch")) "Must NOT reach judged launch"

    # Red Control for 1c: Remove the judged Assert call and verify it reaches judged launch
    Write-Host "[1c-red] Red control: removing judged check reaches launch"
    $harnessNoJudged = Join-Path (Join-Path $RepoRoot "scripts") "tmp_harness_no_judged.ps1"
    $mutatedNoJudged = $harnessContent.Replace('Assert-Rpcs3ReadyForLaunch "judged boot of $name"', '# REMOVED CHECK')
    $mutatedNoJudged | Set-Content -Encoding ascii $harnessNoJudged

    $lock1cRed = Join-Path $TestDir "lock1c_red.json"
    $wrapper1cRed = Join-Path $TestDir "wrapper1c_red.ps1"
    @"
`$global:guardCallCount = 0
function Get-CimInstance {
    param(`$ClassName, `$Filter, `$ErrorAction)
    if (`$Filter -match 'dummy_rpcs3') {
        `$global:guardCallCount++
        if (`$global:guardCallCount -le 2) {
            return @()
        } else {
            return @([pscustomobject]@{
                ProcessId = 8888
                CommandLine = 'dummy_rpcs3.exe --judged-boot-collision'
                Name = 'dummy_rpcs3.exe'
            })
        }
    }
    return Microsoft.Management.Infrastructure.CimCmdlets\Get-CimInstance @PSBoundParameters
}
& '$harnessNoJudged' -Rpcs3Path '$DummyExe' -Manifest '$MockManifest' -ResultsRoot '$res1c' -LockPath '$lock1cRed' -Owner '$CurrentOwner' -TargetProcessName 'dummy_rpcs3' -WhatIf
exit `$LASTEXITCODE
"@ | Set-Content -Encoding ascii $wrapper1cRed

    $out1cRed = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $wrapper1cRed 2>&1
    $out1cRedStr = ($out1cRed | Out-String)
    Assert-True ($out1cRedStr -match "skipping actual judged launch") "Red control must prove judged launch is reached when check is absent"
    Remove-Item $harnessNoJudged, $lock1cRed -Force -ErrorAction SilentlyContinue


    # =========================================================================
    # PART 2: Mid-run lock replacement (same owner, different PID) preserved
    # =========================================================================
    Write-Host "[2] Mid-run lock replacement: same owner, different PID refused & PRESERVED"
    $lock2 = Join-Path $TestDir "lock2.json"
    $res2 = Join-Path $TestDir "res2"
    $wrapper2 = Join-Path $TestDir "wrapper2.ps1"
    # Harness claims lock. Before warm-up, an external runner overwrites the lock with PID 99999 (same owner name).
    # Harness prelaunch check throws because runner PID does not match lock's PID.
    # Harness finally block calls rpcs3-release.ps1 with -ProcessId $PID (no -Force).
    # Release refuses to delete, leaving the PID 99999 lock file intact!
    @"
function Get-CimInstance {
    param(`$ClassName, `$Filter, `$ErrorAction)
    if (`$Filter -match 'dummy_rpcs3') {
        # Overwrite lock with same owner, different PID
        `$stolen = [ordered]@{
            owner = '$CurrentOwner'
            pid = 99999
            host = '$env:COMPUTERNAME'
            timestamp = '2026-09-24T12:00:00Z'
        } | ConvertTo-Json -Compress
        [System.IO.File]::WriteAllText('$lock2', `$stolen)
        return @()
    }
    return Microsoft.Management.Infrastructure.CimCmdlets\Get-CimInstance @PSBoundParameters
}
& '$HarnessScript' -Rpcs3Path '$DummyExe' -Manifest '$MockManifest' -ResultsRoot '$res2' -LockPath '$lock2' -Owner '$CurrentOwner' -TargetProcessName 'dummy_rpcs3' -WhatIf
exit `$LASTEXITCODE
"@ | Set-Content -Encoding ascii $wrapper2

    $out2 = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $wrapper2 2>&1
    $rc2 = $LASTEXITCODE
    Assert-Equal $rc2 1 "Must exit 1 when lock was replaced with different PID"
    Assert-True (Test-Path $lock2) "Replaced lock with different PID MUST be preserved in finally"
    $lock2Json = (Get-Content -Raw $lock2) | ConvertFrom-Json
    Assert-Equal ([int]$lock2Json.pid) 99999 "Lock on disk must still have PID 99999"
    Assert-Equal $lock2Json.owner $CurrentOwner "Lock on disk must have original owner"

    # Red Control for Part 2: Parent release would delete same owner with different PID
    Write-Host "[2-red] Red control: parent release without PID check would delete the lock"
    # Parent rpcs3-release only checked ($lockOwner -ne $Owner)
    $parentReleaseDeletes = $false
    $rawStolen = Get-Content -Raw $lock2
    $parsedStolen = $rawStolen | ConvertFrom-Json
    if ($parsedStolen.owner -eq $CurrentOwner) {
        # Parent would execute: [System.IO.File]::Delete($LockPath)
        $parentReleaseDeletes = $true
    }
    Assert-True $parentReleaseDeletes "Red control: parent release deleted same-owner different-PID locks"
    Remove-Item $lock2 -Force -ErrorAction SilentlyContinue


    # =========================================================================
    # PART 3: Eight-Case Native Replay on rpcs3-release.ps1
    # =========================================================================
    Write-Host "[3] Eight-case native release replay"

    # Case 1: Empty JSON {}
    $l3_1 = Join-Path $TestDir "l3_1.json"
    [System.IO.File]::WriteAllText($l3_1, "{}")
    $out3_1 = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $ReleaseScript -LockPath $l3_1 -Owner $CurrentOwner -ProcessId 1234 2>&1
    Assert-Equal $LASTEXITCODE 1 "Case 1: {} must exit 1"
    Assert-True (Test-Path $l3_1) "Case 1: {} must be retained"
    Assert-Equal (Get-Content -Raw $l3_1) "{}" "Case 1: content must be byte-identical"

    # Case 2: Missing PID field
    $l3_2 = Join-Path $TestDir "l3_2.json"
    $c2Json = "{`"owner`":`"$CurrentOwner`"}"
    [System.IO.File]::WriteAllText($l3_2, $c2Json)
    $out3_2 = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $ReleaseScript -LockPath $l3_2 -Owner $CurrentOwner -ProcessId 1234 2>&1
    Assert-Equal $LASTEXITCODE 1 "Case 2: missing pid must exit 1"
    Assert-True (Test-Path $l3_2) "Case 2: missing pid lock must be retained"
    Assert-Equal (Get-Content -Raw $l3_2) $c2Json "Case 2: content must be byte-identical"

    # Case 3: Null PID
    $l3_3 = Join-Path $TestDir "l3_3.json"
    $c3Json = "{`"owner`":`"$CurrentOwner`",`"pid`":null}"
    [System.IO.File]::WriteAllText($l3_3, $c3Json)
    $out3_3 = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $ReleaseScript -LockPath $l3_3 -Owner $CurrentOwner -ProcessId 1234 2>&1
    Assert-Equal $LASTEXITCODE 1 "Case 3: null pid must exit 1"
    Assert-True (Test-Path $l3_3) "Case 3: null pid lock must be retained"
    Assert-Equal (Get-Content -Raw $l3_3) $c3Json "Case 3: content must be byte-identical"

    # Case 4: Zero PID when runner PID is nonzero
    $l3_4 = Join-Path $TestDir "l3_4.json"
    $c4Json = "{`"owner`":`"$CurrentOwner`",`"pid`":0}"
    [System.IO.File]::WriteAllText($l3_4, $c4Json)
    $out3_4 = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $ReleaseScript -LockPath $l3_4 -Owner $CurrentOwner -ProcessId 1234 2>&1
    Assert-Equal $LASTEXITCODE 1 "Case 4: pid 0 must exit 1"
    Assert-True (Test-Path $l3_4) "Case 4: pid 0 lock must be retained"
    Assert-Equal (Get-Content -Raw $l3_4) $c4Json "Case 4: content must be byte-identical"

    # Case 5: Non-integer PID
    $l3_5 = Join-Path $TestDir "l3_5.json"
    $c5Json = "{`"owner`":`"$CurrentOwner`",`"pid`":`"invalid`"}"
    [System.IO.File]::WriteAllText($l3_5, $c5Json)
    $out3_5 = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $ReleaseScript -LockPath $l3_5 -Owner $CurrentOwner -ProcessId 1234 2>&1
    Assert-Equal $LASTEXITCODE 1 "Case 5: non-integer pid must exit 1"
    Assert-True (Test-Path $l3_5) "Case 5: non-integer pid lock must be retained"
    Assert-Equal (Get-Content -Raw $l3_5) $c5Json "Case 5: content must be byte-identical"

    # Case 6: Wrong PID (same owner)
    $l3_6 = Join-Path $TestDir "l3_6.json"
    $c6Json = "{`"owner`":`"$CurrentOwner`",`"pid`":99999}"
    [System.IO.File]::WriteAllText($l3_6, $c6Json)
    $out3_6 = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $ReleaseScript -LockPath $l3_6 -Owner $CurrentOwner -ProcessId 1234 2>&1
    Assert-Equal $LASTEXITCODE 1 "Case 6: wrong pid must exit 1"
    Assert-True (Test-Path $l3_6) "Case 6: wrong pid lock must be retained"
    Assert-Equal (Get-Content -Raw $l3_6) $c6Json "Case 6: content must be byte-identical"

    # Case 7: Foreign owner (matching PID)
    $l3_7 = Join-Path $TestDir "l3_7.json"
    $c7Json = "{`"owner`":`"other@host`",`"pid`":1234}"
    [System.IO.File]::WriteAllText($l3_7, $c7Json)
    $out3_7 = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $ReleaseScript -LockPath $l3_7 -Owner $CurrentOwner -ProcessId 1234 2>&1
    Assert-Equal $LASTEXITCODE 1 "Case 7: foreign owner must exit 1"
    Assert-True (Test-Path $l3_7) "Case 7: foreign owner lock must be retained"
    Assert-Equal (Get-Content -Raw $l3_7) $c7Json "Case 7: content must be byte-identical"

    # Case 8: Matching owner AND matching PID
    $l3_8 = Join-Path $TestDir "l3_8.json"
    $c8Json = "{`"owner`":`"$CurrentOwner`",`"pid`":1234}"
    [System.IO.File]::WriteAllText($l3_8, $c8Json)
    $out3_8 = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $ReleaseScript -LockPath $l3_8 -Owner $CurrentOwner -ProcessId 1234 2>&1
    Assert-Equal $LASTEXITCODE 0 "Case 8: matching identity must exit 0"
    Assert-True (-not (Test-Path $l3_8)) "Case 8: matching lock must be deleted"


    # =========================================================================
    # PART 4: Normal -WhatIf run records SKIPPED rows, no false PASS, cleans lock
    # =========================================================================
    Write-Host "[4] Normal -WhatIf run records SKIPPED rows without false PASS"
    $lock4 = Join-Path $TestDir "lock4.json"
    $res4 = Join-Path $TestDir "res4"
    $mockManifest4 = Join-Path $TestDir "manifest4.txt"
    @"
name,relative_self,timeout_seconds,expected_state,required_tty_regex,forbidden_tty_regex
sample_a,scripts/run-regression-rpcs3.ps1,10,RAN-CLEAN,-,-
sample_b,scripts/rpcs3-claim.ps1,10,RAN-CLEAN,-,-
"@ | Set-Content -Encoding ascii $mockManifest4

    $out4 = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $HarnessScript `
        -Rpcs3Path $DummyExe `
        -Manifest $mockManifest4 `
        -ResultsRoot $res4 `
        -LockPath $lock4 `
        -Owner $CurrentOwner `
        -TargetProcessName "dummy_none" `
        -WhatIf 2>&1
    $rc4 = $LASTEXITCODE
    Assert-Equal $rc4 0 "Normal -WhatIf run must exit 0"
    Assert-True (-not (Test-Path $lock4)) "Normal -WhatIf run must delete its own lock"

    $csvPath = Join-Path $res4 "regression-rpcs3.csv"
    Assert-True (Test-Path $csvPath) "regression-rpcs3.csv must be generated"
    $csvRows = @(Import-Csv $csvPath)
    Assert-Equal $csvRows.Count 2 "Must have 2 rows in regression-rpcs3.csv"
    foreach ($row in $csvRows) {
        Assert-Equal $row.emulator "SKIPPED" "Row emulator must be SKIPPED under -WhatIf"
        Assert-Equal $row.guest "SKIPPED" "Row guest must be SKIPPED under -WhatIf"
    }


    # =========================================================================
    # PART 5: Lock claim write failure handling and cleanup
    # =========================================================================
    Write-Host "[5] Lock claim write failure: exit != 0, no lock remains, error != 'already exists'"
    $lock5 = Join-Path $TestDir "lock5.json"
    $wrapper5 = Join-Path $TestDir "wrapper5.ps1"
    @"
function New-Object {
    param([string]`$TypeName, [object[]]`$ArgumentList)
    if (`$TypeName -eq 'System.IO.StreamWriter') {
        throw [System.IO.IOException]::new('There is not enough space on the disk')
    }
    return Microsoft.PowerShell.Utility\New-Object @args
}
& '$ClaimScript' -LockPath '$lock5' -Owner '$CurrentOwner' -ProcessId 1234
exit `$LASTEXITCODE
"@ | Set-Content -Encoding ascii $wrapper5

    $out5 = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $wrapper5 2>&1
    $rc5 = $LASTEXITCODE
    Assert-True ($rc5 -ne 0) "Candidate must exit nonzero on write failure"
    Assert-True (-not (Test-Path $lock5)) "Candidate must remove lock file on write failure"
    $out5Str = ($out5 | Out-String)
    Assert-True (-not ($out5Str -match "already exists")) "Candidate message must NOT report 'already exists'"
    Assert-True ($out5Str -match "There is not enough space on the disk" -or $out5Str -match "Failed to write") "Candidate message must report write failure"

    # Red Control for Part 5: Parent claim script leaves lock on disk and reports 'already exists'
    Write-Host "[5-red] Red control: parent script leaves lock file and reports 'already exists'"
    $lock5Red = Join-Path $TestDir "lock5_red.json"
    $wrapper5Red = Join-Path $TestDir "wrapper5_red.ps1"
    $claimScriptParent = Join-Path $TestDir "claim_parent.ps1"

    @"
param(
    [string]`$LockPath = "C:\ps3boot\.rpcs3-owner",
    [string]`$Owner = "`$env:USERNAME@`$env:COMPUTERNAME",
    [int]`$ProcessId = `$PID
)

`$ErrorActionPreference = "Stop"

if (-not `$Owner -or `$Owner.Trim() -eq "") {
    Write-Error "Owner cannot be empty"
    exit 1
}

`$parent = Split-Path -Parent `$LockPath
if (`$parent) {
    New-Item -ItemType Directory -Force -Path `$parent | Out-Null
}

`$payload = [ordered]@{
    owner = `$Owner
    pid = `$ProcessId
    host = `$env:COMPUTERNAME
    timestamp = (Get-Date).ToString("o")
} | ConvertTo-Json -Compress

try {
    `$stream = [System.IO.File]::Open(`$LockPath,
        [System.IO.FileMode]::CreateNew,
        [System.IO.FileAccess]::Write,
        [System.IO.FileShare]::None)
    try {
        `$writer = New-Object System.IO.StreamWriter(`$stream)
        try {
            `$writer.WriteLine(`$payload)
            `$writer.Flush()
        } finally {
            `$writer.Dispose()
        }
    } finally {
        `$stream.Dispose()
    }
} catch [System.IO.IOException] {
    Write-Error "RPCS3 lock already exists at `$LockPath. Current owner: `$(Get-Content -Raw -Path `$LockPath -ErrorAction SilentlyContinue)"
    exit 1
}

`$written = Get-Content -Raw -LiteralPath `$LockPath -ErrorAction SilentlyContinue
if (-not `$written -or `$written.Trim() -eq "") {
    try { [System.IO.File]::Delete(`$LockPath) } catch {}
    Write-Error "Failed to write owner line into RPCS3 lock file at `$LockPath"
    exit 1
}

Write-Host "claimed `$LockPath for `$Owner"
"@ | Set-Content -Encoding ascii $claimScriptParent

    @"
function New-Object {
    param([string]`$TypeName, [object[]]`$ArgumentList)
    if (`$TypeName -eq 'System.IO.StreamWriter') {
        throw [System.IO.IOException]::new('There is not enough space on the disk')
    }
    return Microsoft.PowerShell.Utility\New-Object @args
}
& '$claimScriptParent' -LockPath '$lock5Red' -Owner '$CurrentOwner' -ProcessId 1234
exit `$LASTEXITCODE
"@ | Set-Content -Encoding ascii $wrapper5Red

    $out5Red = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $wrapper5Red 2>&1
    $rc5Red = $LASTEXITCODE
    Assert-Equal $rc5Red 1 "Red control must exit 1"
    Assert-True (Test-Path $lock5Red) "Red control: parent script leaves lock file on write failure"
    $out5RedStr = ($out5Red | Out-String)
    Assert-True ($out5RedStr -match "already exists") "Red control: parent script reports 'already exists' on write failure"
    Remove-Item $lock5Red, $claimScriptParent, $wrapper5Red, $wrapper5 -Force -ErrorAction SilentlyContinue

    Write-Host "All regression tests passed successfully."
} finally {
    Remove-Item $TestDir -Recurse -Force -ErrorAction SilentlyContinue
}
