param(
    [string]$LockPath = "C:\ps3boot\.rpcs3-owner",
    [string]$Owner = "$env:USERNAME@$env:COMPUTERNAME",
    [int]$ProcessId = $PID
)

$ErrorActionPreference = "Stop"

if (-not $Owner -or $Owner.Trim() -eq "") {
    Write-Error "Owner cannot be empty"
    exit 1
}

$parent = Split-Path -Parent $LockPath
if ($parent) {
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
}

$payload = [ordered]@{
    owner = $Owner
    pid = $ProcessId
    host = $env:COMPUTERNAME
    timestamp = (Get-Date).ToString("o")
} | ConvertTo-Json -Compress

$stream = $null
try {
    $stream = [System.IO.File]::Open($LockPath,
        [System.IO.FileMode]::CreateNew,
        [System.IO.FileAccess]::Write,
        [System.IO.FileShare]::None)
} catch [System.IO.IOException] {
    if ([System.IO.File]::Exists($LockPath)) {
        Write-Error "RPCS3 lock already exists at $LockPath. Current owner: $(Get-Content -Raw -LiteralPath $LockPath -ErrorAction SilentlyContinue)"
        exit 1
    }
    throw
}

try {
    try {
        $writer = New-Object System.IO.StreamWriter($stream)
        try {
            $writer.WriteLine($payload)
            $writer.Flush()
        } finally {
            $writer.Dispose()
        }
    } finally {
        $stream.Dispose()
    }
} catch {
    Remove-Item -LiteralPath $LockPath -ErrorAction Stop
    throw
}

$written = Get-Content -Raw -LiteralPath $LockPath -ErrorAction SilentlyContinue
if (-not $written -or $written.Trim() -eq "") {
    Remove-Item -LiteralPath $LockPath -Force -ErrorAction SilentlyContinue
    Write-Error "Failed to write owner line into RPCS3 lock file at $LockPath"
    exit 1
}

Write-Host "claimed $LockPath for $Owner"
