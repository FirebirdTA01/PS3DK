param(
    [string]$LockPath = "C:\ps3boot\.rpcs3-owner",
    [string]$Owner = "$env:USERNAME@$env:COMPUTERNAME",
    [int]$ProcessId = 0,
    [switch]$Force
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $LockPath)) {
    Write-Host "no RPCS3 lock at $LockPath"
    exit 0
}

$raw = Get-Content -Raw -LiteralPath $LockPath
$lockOwner = $null
$lockPid = $null
if ($raw -and $raw.Trim() -ne "") {
    try {
        $json = $raw | ConvertFrom-Json
        $lockOwner = $json.owner
        $lockPid = $json.pid
    } catch {
        if (-not $Force) {
            Write-Error "RPCS3 lock at $LockPath is not valid JSON; use -Force to remove it. Content: $raw"
            exit 1
        }
    }
} else {
    if (-not $Force) {
        Write-Error "RPCS3 lock at $LockPath is empty; use -Force to remove it."
        exit 1
    }
}

if (-not $Force) {
    if ([string]::IsNullOrWhiteSpace($lockOwner)) {
        Write-Error "RPCS3 lock at $LockPath has no owner; use -Force to remove it."
        exit 1
    }
    if ($lockOwner -ne $Owner) {
        Write-Error "RPCS3 lock at $LockPath is owned by '$lockOwner', not '$Owner'. Use -Force only after coordinating."
        exit 1
    }
    if ($ProcessId -gt 0) {
        $parsedPid = 0
        if ($null -eq $lockPid -or -not [int]::TryParse("$lockPid", [ref]$parsedPid)) {
            Write-Error "RPCS3 lock at $LockPath has missing or non-integer PID '$lockPid' (expected $ProcessId); use -Force to remove it."
            exit 1
        }
        if ($parsedPid -ne $ProcessId) {
            Write-Error "RPCS3 lock at $LockPath was claimed by PID $parsedPid, not runner PID $ProcessId. Use -Force only after coordinating."
            exit 1
        }
    }
}

[System.IO.File]::Delete($LockPath)
Write-Host "released $LockPath"
