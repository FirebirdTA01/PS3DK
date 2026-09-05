param(
    [string]$RepoRoot = "",
    [string]$Manifest = "",
    [string]$Rpcs3Path = "C:\Users\FirebirdTA01\Desktop\Emulators\RPCS3\rpcs3.exe",
    [string]$ResultsRoot = "",
    [string]$LockPath = "C:\ps3boot\.rpcs3-owner",
    [string]$Owner = "regression-rpcs3@$env:COMPUTERNAME"
)

$ErrorActionPreference = "Stop"

if (-not $RepoRoot) {
    $RepoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
}
$RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path
if (-not $Manifest) {
    $Manifest = Join-Path $RepoRoot "tests\regression\manifest.txt"
}
if (-not $ResultsRoot) {
    $ResultsRoot = Join-Path "C:\ps3regression" ("run-" + (Get-Date -Format "yyyyMMdd-HHmmss"))
}

if (-not (Test-Path -LiteralPath $Rpcs3Path)) {
    throw "missing RPCS3 executable: $Rpcs3Path"
}

$rpcs3Dir = Split-Path -Parent $Rpcs3Path
$logDir = Join-Path $rpcs3Dir "log"
$rpcs3Log = Join-Path $logDir "RPCS3.log"
$ttyLog = Join-Path $logDir "TTY.log"
$fatalRegex = "(^|[ \u00B7])F |Fatal|Access violation|frozen|Dead FIFO|recover_fifo|runtime_error|Emulation has been frozen"
$shaderMetricHelper = Join-Path $RepoRoot "tests\regression\shader-differential\container-metrics.ps1"
if (Test-Path -LiteralPath $shaderMetricHelper -PathType Leaf) {
    . $shaderMetricHelper
}

New-Item -ItemType Directory -Force -Path $ResultsRoot | Out-Null

$claimScript = Join-Path $RepoRoot "scripts\rpcs3-claim.ps1"
$releaseScript = Join-Path $RepoRoot "scripts\rpcs3-release.ps1"

& powershell -NoProfile -ExecutionPolicy Bypass -File $claimScript -LockPath $LockPath -Owner $Owner
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

$exitCode = 0
try {
    if (Get-Process -Name "rpcs3" -ErrorAction SilentlyContinue) {
        throw "rpcs3.exe is already running"
    }

    if (Test-Path -LiteralPath $logDir) {
        $preserveDir = Join-Path $ResultsRoot ("preserved-before-run-" + (Get-Date -Format "yyyyMMdd-HHmmss"))
        Copy-Item -LiteralPath $logDir -Destination $preserveDir -Recurse -Force
    } else {
        New-Item -ItemType Directory -Force -Path $logDir | Out-Null
    }

    # Lines starting with '#' are comments (deliberate-encoding notes live
    # next to the rows they explain); strip them, and blank lines — which
    # ConvertFrom-Csv would otherwise turn into all-null phantom rows —
    # before CSV parsing.
    $rows = Get-Content -LiteralPath $Manifest | Where-Object { $_ -notmatch '^\s*(#|$)' } | ConvertFrom-Csv
    $results = @()

    foreach ($row in $rows) {
        $name = $row.name
        $selfPath = Join-Path $RepoRoot $row.relative_self
        $sampleDir = Join-Path $ResultsRoot $name
        New-Item -ItemType Directory -Force -Path $sampleDir | Out-Null

        if (-not (Test-Path -LiteralPath $selfPath)) {
            $results += [pscustomobject]@{
                name = $name
                self = $row.relative_self
                emulator = "missing_self"
                guest = "FAIL"
                warmed_up = $false
                tty_lines = 0
                forbidden_tty_hits = 0
                first_forbidden_tty = ""
                fatal_hits = 0
                first_fatal = ""
                log_dir = $sampleDir
            }
            $exitCode = 1
            continue
        }

        $timeout = [int]$row.timeout_seconds

        # t_f4155031: a cold PPU LLVM cache presents exactly like a real
        # failure (no TTY + timeout) because RPCS3 spends the whole window
        # compiling instead of running — every self recompile (and any
        # codegen change recompiles ALL of them) invalidates the cache.
        # Detection: RPCS3 keys cache dirs as cache/ppu-<hash>-<basename>,
        # and the hash tracks the binary's content, so a REBUILT self gets
        # a fresh hash while its stale same-name dir lingers.  A bare
        # name match therefore proves nothing; "some name-matching cache
        # dir was CREATED AFTER the binary was last written" is the
        # warm signal.  ERROR DIRECTION, before anyone "optimises away
        # the redundant boot": a false COLD (dir exists but predates a
        # byte-identical rebuild) costs one extra unjudged boot —
        # harmless.  A false WARM needs a SAME-NAMED, DIFFERENT-CONTENT
        # binary booted more recently than this one (e.g. from another
        # checkout); then the judged run eats the compile and can
        # report the mystery red this check exists to prevent — the
        # in-harness draw retries and the row timeout are the only
        # backstop there.  The heuristic leans cold on purpose.
        # The warm-up boot is UNJUDGED: it exists to bank the PPU
        # compile; its logs are truncated away before the judged run.
        $didWarmup = $false
        $cacheRoot = Join-Path $rpcs3Dir "cache"
        $selfLeaf = Split-Path -Leaf $selfPath
        $selfTime = (Get-Item -LiteralPath $selfPath).LastWriteTimeUtc
        $warmDirs = @()
        if (Test-Path -LiteralPath $cacheRoot) {
            $warmDirs = @(Get-ChildItem -LiteralPath $cacheRoot -Directory -ErrorAction SilentlyContinue |
                Where-Object { $_.Name -like "ppu-*-$selfLeaf" -and $_.CreationTimeUtc -gt $selfTime })
        }
        if ($warmDirs.Count -eq 0) {
            $didWarmup = $true
            $warmTimeout = [Math]::Max(120, $timeout * 2)
            Write-Host "  ${name}: PPU cache cold for this binary - unjudged warm-up boot (ceiling ${warmTimeout}s)"
            $warmProc = Start-Process -FilePath $Rpcs3Path -ArgumentList @("--no-gui", $selfPath) -WorkingDirectory $rpcs3Dir -WindowStyle Hidden -PassThru
            if (-not $warmProc.WaitForExit($warmTimeout * 1000)) {
                Stop-Process -Id $warmProc.Id -Force
                $warmProc.WaitForExit()
            }
            Start-Sleep -Milliseconds 300
        }

        Set-Content -LiteralPath $rpcs3Log -Value ""
        Set-Content -LiteralPath $ttyLog -Value ""
        $proc = Start-Process -FilePath $Rpcs3Path -ArgumentList @("--no-gui", $selfPath) -WorkingDirectory $rpcs3Dir -WindowStyle Hidden -PassThru
        $exited = $proc.WaitForExit($timeout * 1000)
        if ($exited) {
            $emulator = "exit_$($proc.ExitCode)"
        } else {
            $emulator = "ran_${timeout}s"
            Stop-Process -Id $proc.Id -Force
            $proc.WaitForExit()
        }

        Start-Sleep -Milliseconds 300

        $copiedRpcs3Log = Join-Path $sampleDir "RPCS3.log"
        $copiedTtyLog = Join-Path $sampleDir "TTY.log"
        Copy-Item -LiteralPath $rpcs3Log -Destination $copiedRpcs3Log -Force
        Copy-Item -LiteralPath $ttyLog -Destination $copiedTtyLog -Force

        $ttyText = Get-Content -Raw -LiteralPath $copiedTtyLog -ErrorAction SilentlyContinue
        $rpcs3Text = Get-Content -Raw -LiteralPath $copiedRpcs3Log -ErrorAction SilentlyContinue
        $ttyLines = @($ttyText -split "`r?`n" | Where-Object { $_.Length -gt 0 }).Count

        if ((Get-Command Parse-SdiffRows -ErrorAction SilentlyContinue) -and
            (Get-Command Join-ContainerMetricsWithSdiff -ErrorAction SilentlyContinue) -and
            (Get-Command Write-ContainerMetricsReport -ErrorAction SilentlyContinue) -and
            [regex]::IsMatch($ttyText, "(?m)^SDIFF\|")) {
            $stagedMetrics = Join-Path $rpcs3Dir "dev_hdd0\shader-differential\container-metrics.csv"
            if (Test-Path -LiteralPath $stagedMetrics -PathType Leaf) {
                $metricRows = @(Import-Csv -LiteralPath $stagedMetrics)
                $sdiffRows = Parse-SdiffRows $ttyText
                $joinedRows = Join-ContainerMetricsWithSdiff $metricRows $sdiffRows
                Write-ContainerMetricsReport $joinedRows (Join-Path $sampleDir "container-metrics.csv")
            }
        }

        $required = $row.required_tty_regex
        $forbidden = $row.forbidden_tty_regex
        if ($null -eq $forbidden -or $forbidden -eq "") {
            $forbidden = "ENOSYS"
        }

        $requiredOk = $true
        if ($required -and $required -ne "-") {
            $requiredOk = [regex]::IsMatch($ttyText, $required)
        }

        $forbiddenMatches = @()
        if ($forbidden -and $forbidden -ne "-") {
            $forbiddenMatches = [regex]::Matches($ttyText, $forbidden)
        }

        $fatalLines = @($rpcs3Text -split "`r?`n" |
            Where-Object { $_ -match $fatalRegex -and $_ -notmatch "Show fatal error hints" })

        $guest = "PASS"
        if ($forbiddenMatches.Count -gt 0) {
            $guest = "FAIL"
        } elseif (-not $requiredOk) {
            $guest = "UNCLASSIFIED"
        } elseif ($ttyLines -eq 0) {
            $guest = "NO_TTY"
        }

        if ($row.expected_state -eq "RAN-CLEAN" -and $emulator -notmatch "^exit_") {
            $exitCode = 1
        }
        if ($row.expected_state -eq "RENDER-LOOP" -and $emulator -notmatch "^ran_") {
            $exitCode = 1
        }
        if ($guest -ne "PASS" -or $fatalLines.Count -gt 0) {
            $exitCode = 1
        }

        $results += [pscustomobject]@{
            name = $name
            self = $row.relative_self
            emulator = $emulator
            guest = $guest
            warmed_up = $didWarmup
            tty_lines = $ttyLines
            forbidden_tty_hits = $forbiddenMatches.Count
            first_forbidden_tty = if ($forbiddenMatches.Count -gt 0) { $forbiddenMatches[0].Value } else { "" }
            fatal_hits = $fatalLines.Count
            first_fatal = if ($fatalLines.Count -gt 0) { $fatalLines[0] } else { "" }
            log_dir = $sampleDir
        }
    }

    $csv = Join-Path $ResultsRoot "regression-rpcs3.csv"
    $results | Export-Csv -NoTypeInformation -Path $csv
    Write-Host "runtime regression results: $csv"
} finally {
    & powershell -NoProfile -ExecutionPolicy Bypass -File $releaseScript -LockPath $LockPath -Owner $Owner
}

exit $exitCode
