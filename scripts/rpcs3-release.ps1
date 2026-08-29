param(
    [string]$LockPath = "C:\ps3boot\.rpcs3-owner",
    [string]$Owner = "$env:USERNAME@$env:COMPUTERNAME",
    [switch]$Force
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $LockPath)) {
    Write-Host "no RPCS3 lock at $LockPath"
    exit 0
}

$raw = Get-Content -Raw -LiteralPath $LockPath
$lockOwner = $null
try {
    $lockOwner = ($raw | ConvertFrom-Json).owner
} catch {
    if (-not $Force) {
        Write-Error "RPCS3 lock at $LockPath is not valid JSON; use -Force to remove it. Content: $raw"
        exit 1
    }
}

if (-not $Force -and $lockOwner -and $lockOwner -ne $Owner) {
    Write-Error "RPCS3 lock at $LockPath is owned by '$lockOwner', not '$Owner'. Use -Force only after coordinating."
    exit 1
}

[System.IO.File]::Delete($LockPath)
Write-Host "released $LockPath"
